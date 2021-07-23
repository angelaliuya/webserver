#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/lywebserver/resources";

//设置非阻塞
int setnonblocking( int fd ) {
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
//addfd( epollfd, listenfd, false ); //监听套接字
//addfd( m_epollfd, sockfd, true ); //连接套接字
void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP; //EPOLLRDHUP:通过事件判断对端是否断开
    if(one_shot) 
    {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
        //只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里。
    }
    /*
       epoll的事件注册函数:
       epoll的句柄；
       动作类型：注册新的fd到epollfd
       需要监听的fd;
       告诉内核需要监听什么事；
    */
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);  
}

// 从epoll中移除监听的文件描述符
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}
/*
   当响应结束时：modfd( m_epollfd, m_sockfd, EPOLLIN );再次触发读
*/
// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    //修改已经注册的fd的监听事件
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP; //设置为边沿触发，
    //将EPOLL设为边缘触发；
    /*
       当被监控的文件描述符上有可读写事件发生时，epoll_wait会通知处理程序
       去读写，如果这次没有把数据全部读写完，下次调用epoll_wait时，它不会通知你，直到
       该文件描述符上出席那第二次读写事件事才会通知你。
    */
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;  //是都用一个epollfd吗

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}
/*
   users[connfd].init( connfd, client_address);
*/
// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;  //客户端的sockfd
    m_address = addr;   //客户端的ip地址
    
    // 端口复用
    int reuse = 1;
    /*
    获取或设置与某个套接字关联的选项：
    将要被设置或者获取选项的套接字；
    选项所在的协议层；//控制套接字的层次：SOL_SOCKET（通用套接字选项）；IPPROTO_IP:IP选项.)IPPROTO_TCP:TCP选项.　
    选项的名称；
    指向包含新选项值的缓冲；
    选项值的长度；
    SO_REUSEADDR：允许重用本地地址和端口　　
    */
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    addfd( m_epollfd, sockfd, true );
    m_user_count++;
    init();
}

void http_conn::init()
{
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              // 获取一个目标URL        
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    if( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }
    int bytes_read = 0;
    /*
    协议接收的数据可能大于buf的长度，所以在这种情况下要调用几次recv函数才能把套接字接收缓冲区中的数据copy完
    如果是阻塞套接字，套接字缓冲区中没有任何数据时，recv就会等待，直到数据>=1, 
    如果是非阻塞套接字，网卡缓冲区中没有数据时，recv也会立即返回。
    非阻塞必须循环读取，确保读尽
    */
   /*使用while循环的原因
     边缘触发的情况下，如果不一次性读取一个事件上的数据，会干扰下一个事件
     所以必须在读取数据的外部套一层循环，这样才能完整的读取数据
   */
    while(true) {  //非阻塞套接字，recv是非阻塞的  如果应用层的数组满了怎么办 ？？？
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
        READ_BUFFER_SIZE - m_read_idx, 0 );
        /*如果是阻塞IO，处理完数据后，程序会一直卡在recv上，因为是阻塞IO，如果没数据可读，它会一直等在那，
        直到有数据可读。但是这个时候，如果有另外一个客户端取连接服务器，服务器就不能受理这个新的客户端了。
        */
       /*
          所以把socket设置为非阻塞，没有数据可读时，会立即返回，并设置errno.
          在边沿模式下，必须是非阻塞的
       */

        if (bytes_read == -1) {
            /*
            //EAGIN,又叫EWOULDBLOCK ,提示应用程序现在没有数据可读，请稍后再试
            */
            if( errno == EAGAIN || errno == EWOULDBLOCK ) { 
                // 没有数据
                break;//退出循环
            }
            return false;   //出错
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

// 解析一行，判断依据\r\n,每一行都是以回车换行符结束
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) {
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                return LINE_OPEN;  //(以\r结尾，没有换行符)行数据尚且不完整
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) { //\r\n
                m_read_buf[ m_checked_idx++ ] = '\0'; //把\r->\0   //标志着这是一行
                m_read_buf[ m_checked_idx++ ] = '\0'; //把\n->\0   //m_checked_idx此时指向\n的下一个字符
                return LINE_OK;  //读取到一个完整的行
            }
            return LINE_BAD;   //行出错
        } else if( temp == '\n' )  {
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {//若返回空
        return BAD_REQUEST;
    }
    *m_version++ = '\0';  //把\t->\0,向前走一步
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;  //忽略大小写比较字符串，相同返回0
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;// 请求不完整，需要继续读取客户数据,需要继续读取头
}

// 解析HTTP请求的一个头部信息（字符串匹配，记录下一些一些字段的值）
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {  //那一行为\0
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );//其他的一切看作未知的
    }
    return NO_REQUEST;  //没有换状态
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    //初始化，行
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    /*
        parse_line:从数组中读取一行，返回读取一行的三种状态
    */
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) { //根据\r\n获取一行数据，置为'\0'，并得到读完该行的状态
        // 获取一行数据
        text = get_line(); //获取当前行的开始索引
        m_start_line = m_checked_idx;//下一行的开始索引
        printf( "got 1 http line: %s\n", text );
        /*
           m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
        */
        //先读取一行文本，根据主状态机的状态去分别进行不同的处理
        switch ( m_check_state ) { 
            case CHECK_STATE_REQUESTLINE: { //检查请求行
                ret = parse_request_line( text ); //检查状态变成检查头,return NO_REQUEST.
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: { //检查请求头部字段
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {   
                    return do_request();  //请求解析完了，去回复
                }
                break;
            }
            case CHECK_STATE_CONTENT: {//当前正在解析请求体
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request(); //没有真正解析消息体，去回复
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/nowcoder/webserver/resources"
    strcpy( m_real_file, doc_root ); // 字符串复制 b->a
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    //通过文件名filename获取文件信息，并保存在buf所指的结构体stat中   
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;//表示客户对资源没有足够的访问权限
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {    
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射 文件被映射到内存的起始位置,将一个文件或者其他对象映射进内存
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST; //获取文件成功
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write() 
{
    int temp = 0;
    //int bytes_have_send = 0;    // 已经发送的字节
    //int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    //已经准备好的数据初始化 bytes_to_send = m_write_idx + m_file_stat.st_size;
    //增加一些判断
    //1. 判断响应头是否发送完毕，如果发送完毕了，要做如下处理
    if(bytes_have_send >= m_iv[0].iov_len){
        //头已经发送完毕
        m_iv[0].iov_len = 0;
        m_iv[1].iov_base = m_file_address+(bytes_have_send-m_write_idx);
        m_iv[1].iov_len = bytes_to_send;
    }
    //如果没有发送完毕，还要修改下次写数据的位置
    else{
        m_iv[0].iov_base = m_write_buf + bytes_have_send;
        m_iv[0].iov_len = m_iv[0].iov_len -bytes_have_send;
    }

    if ( bytes_to_send <= 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        /*
           event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP; 
           //边沿触发，ONESHOT事件，对端连接断开会触发EPOLLRDHUP
        */

        modfd( m_epollfd, m_sockfd, EPOLLIN ); //手动触发读
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);//和write相同，除了数据取自IOVEC，而不是连续缓冲区
        if ( temp <= -1 ) {
            // 如果TCP socket写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT ); //再次触发写
                return true;
            }
            //出错
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap(); // 
            if(m_linger) { //HTTP请求是否要求保持连接
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN ); //继续监测读事件
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

// 往写缓冲中写入待发送的数据
//add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list; //定义临时变量
    va_start( arg_list, format ); //指定位置，把这个“空指针”指定到我们需要的位置上
    //将输出格式化输出到一个字符数组中
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );   //指针置空
    return true;
}
//add_status_line( 500, error_500_title );
//add_status_line(200, ok_200_title );
bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;

}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
/*
   HTTP响应报文格式：
   状态行: 协议版本\space状态码\space状态码描述\r\n 
   响应头部
   响应正文
*/
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:  //表示服务器内部错误
            add_status_line( 500, error_500_title ); //写到写缓冲区
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST: //表示文件获取成功
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size); 
            //两个地址，一个是写缓冲区的地址；一个是请求文件映射到内存的地址
            m_iv[ 0 ].iov_base = m_write_buf;//写缓冲区地址
            m_iv[ 0 ].iov_len = m_write_idx;//偏移量
            m_iv[ 1 ].iov_base = m_file_address;// 客户请求的目标文件被mmap到内存中的起始位置 
            m_iv[ 1 ].iov_len = m_file_stat.st_size;//大小
            m_iv_count = 2;
            //响应头的大小+文件的大小，也就是总的要发送的数据
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {//请求不完整，需要继续读取客户数据
        modfd( m_epollfd, m_sockfd, EPOLLIN ); //重新检测读，手动再次触发读 
        return;
    }
    
    // 生成响应 
    //两个地址，一个是写缓冲区的地址，一个是文件被映射到内存中的地址
    //将数据先到缓冲区中
    bool write_ret = process_write( read_ret ); 
    if ( !write_ret ) {
        close_conn();  
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT); //触发写事件，需要触发写时，再把写加入进去
}

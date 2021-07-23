#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include <signal.h>

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符（extern置于函数前,标示函数的定义在别的文件中，提示编译器遇到此函数时在其他模块中寻找其定义。）
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );
//添加信号捕捉
void addsig(int sig, void( handler )(int)){ //处理信号
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );  //所有数据置空
    sa.sa_handler = handler; 
    sigfillset( &sa.sa_mask ); //设置临时阻塞信号集
    //sigaction 功能：检查或修改与指定信号相关联的处理动作
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {//至少要传递端口号
        printf( "usage: %s port_number\n", basename(argv[0])); //获取程序的名称
        return 1;
    }

    int port = atoi( argv[1] ); //字符串转化为整数 获取端口号
    /*
       SIGPIPE:当向一个disconnected socket发送数据时，会让底层抛出一个SIGPIPE信号
    */
    addsig( SIGPIPE, SIG_IGN );  //对SIGPIE信号进行处理，当捕捉到这个信号时，忽略它
    
	//创建线程池，初始化线程池
	
    threadpool< http_conn >* pool = NULL; //任务： http连接任务
    try {
        pool = new threadpool<http_conn>; //创建一个解决http连接任务的线程池
    } catch( ... ) {
        return 1;
    }

    http_conn* users = new http_conn[ MAX_FD ];// 创建多个任务
    //创建一个用于监听的套接字
    /*
       AF_INET： ipv4
       SOCK_STREAM(流式协议): tcp
       protocal(具体的一个协议):0
       返回文件描述符，操作的就是内核缓冲区
       //操作系统会创建一个由文件管理系统管理的socket对象

    */
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 ); //创建一个socket对象

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;  //表示本机所有IP
    address.sin_family = AF_INET;//网络协议
    address.sin_port = htons( port ); //端口号

    // 端口复用
    int reuse = 1; //设置套接字的选项
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    /*
       将这个监听文件描述符与服务器的IP和端口绑定（IP和端口就是服务器的地址信息，也是客户端用来连接的）
    */
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 ); // 设置监听，监听的fd开始工作

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );  //创建一个epoll的句柄
    // 添加到epoll对象中
    addfd( epollfd, listenfd, false ); //把listenfd添加到epollfd,设置为非阻塞
    http_conn::m_epollfd = epollfd; //自始至终都只有一个epollfd 

    while(true) {
        //等待事件的产生，参数events用来从内核得到事件的集合。
        //函数返回需要处理的事件数目，如返回0表示已超时。
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) { //监听到fd
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                //epoll已经检测到事件发生了，所以accept会返回
                /*
                因为 epoll_wait() 返回的必定是已经就绪的连接，
                所以不管是阻塞还是非阻塞，accept() 都会立即返回。
                因为是水平触发，所以不需要while循环来accept()
                */
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    close(connfd);
                    continue;
                }
                users[connfd].init( connfd, client_address);  //拿id,和客户端的地址来初始化一个任务。
                /*
                   初始化所做的事：
                   （1）创建端口复用；
                   （2）向epoll中添加需要监听的文件描述符(非阻塞)
                    (3) 初始化该任务的缓冲区

                */

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {

                //EPOLLHUP：表示对应的文件描述符被挂断;
                //EPOLLERR: 表示对应的文件描述符发生错误；
                //以上两个不需要在epoll_event时针对fd作设置，一样会被触发
                //EPOLLRDHUP:对端关闭时会触发，需要显示地在epoll_ctl调用时，设置在events中

                users[sockfd].close_conn();  //从epollfd中去掉sockfd

            } else if(events[i].events & EPOLLIN) {//表示对应的文件描述符可以读
                //根本没有把异步io模拟出来，这个把数据从内核态拷贝到用户态这个过程还是需要等待的。
                if(users[sockfd].read()) {  //把数据一次性读出来(此时的fd是什么触发方式ET or LT),没有设置，就应该是LT吧？
                    pool->append(users + sockfd); //数据读取这个工作是主线程干的
                } else {
                    users[sockfd].close_conn();
                }

            }  else if( events[i].events & EPOLLOUT ) {//表示对应的文件描述符可以写 ，捕捉到写

                if( !users[sockfd].write() ) { //如果HTTP请求没有要求保持连接，就断开连接
                    users[sockfd].close_conn();
                }

            }
        }
    }
    
    close( epollfd ); // epoll句柄本身会占一个fd的值，使用完epoll,必须调用close关闭。
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}
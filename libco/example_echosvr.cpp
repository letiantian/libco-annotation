/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

/**
 * 设计思路：多进程，每个进程对应一个协程池（协程数量固定），当accept一个fd后，拿出一个协程去处理。
 * 当协程处理完一个fd后，再把自己扔回协程池。 协程是死循环。
 */


#include "co_routine.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <stack>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;
struct task_t
{
	stCoRoutine_t *co;
	int fd;
};

static stack<task_t*> g_readwrite;
static int g_listen_fd = -1;

// 设置为非阻塞
static int SetNonBlock(int iSock)
{
    int iFlags;

    iFlags = fcntl(iSock, F_GETFL, 0);
    iFlags |= O_NONBLOCK;
    iFlags |= O_NDELAY;
    int ret = fcntl(iSock, F_SETFL, iFlags);
    return ret;
}

/**
 * 这个服务器做成了协程池的方式。每个写成
 */
static void *readwrite_routine( void *arg )
{

	co_enable_hook_sys();  // hook 系统函数

	task_t *co = (task_t*)arg;
	char buf[ 1024 * 16 ];
	for(;;)
	{
		if( -1 == co->fd )  // 该协程第一次运行时fd为-1,要等待到fd不为-1
		{
			g_readwrite.push( co );  // 放入g_readwrite
			co_yield_ct();
			continue;
		}

		int fd = co->fd;
		co->fd = -1;  // 进入下次循环时候，又把该协程放入g_readwrite了

		// 开始处理了
		for(;;)
		{
			struct pollfd pf = { 0 };
			pf.fd = fd;
			pf.events = (POLLIN|POLLERR|POLLHUP);
			co_poll( co_get_epoll_ct(),&pf,1,1000);

			int ret = read( fd,buf,sizeof(buf) );
			if( ret > 0 )
			{
				ret = write( fd,buf,ret );
			}
			if( ret <= 0 )
			{
				close( fd );
				break;
			}
		}

	}
	return 0;
}

int co_accept(int fd, struct sockaddr *addr, socklen_t *len );  // 已经被libco hook了

static void *accept_routine( void * )
{
	co_enable_hook_sys();
	printf("accept_routine\n");
	fflush(stdout);
	for(;;)
	{
		//printf("pid %ld g_readwrite.size %ld\n",getpid(),g_readwrite.size());
		if( g_readwrite.empty() ) // 读写协程若没准备好，sleep等一会
		{
			printf("empty\n"); //sleep
			struct pollfd pf = { 0 };
			pf.fd = -1;
			poll( &pf,1,1000);

			continue;

		}
		struct sockaddr_in addr; //maybe sockaddr_un;
		memset( &addr,0,sizeof(addr) );
		socklen_t len = sizeof(addr);

		int fd = co_accept(g_listen_fd, (struct sockaddr *)&addr, &len);
		if( fd < 0 )
		{
			struct pollfd pf = { 0 };
			pf.fd = g_listen_fd;
			pf.events = (POLLIN|POLLERR|POLLHUP);
			co_poll( co_get_epoll_ct(),&pf,1,1000 );
			continue;
		}
		if( g_readwrite.empty() )
		{
			close( fd );
			continue;
		}
		SetNonBlock( fd );
		task_t *co = g_readwrite.top();  // 拿出一个协程处理该fd
		co->fd = fd;
		g_readwrite.pop();   // 先拿出来协程去执行。在readwrite_routine中有机制在执行完后再把协程放入g_readwrite
		co_resume( co->co );
	}
	return 0;
}

// 设置地址
static void SetAddr(const char *pszIP,const unsigned short shPort,struct sockaddr_in &addr)
{
	bzero(&addr,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(shPort);
	int nIP = 0;
	if( !pszIP || '\0' == *pszIP   
	    || 0 == strcmp(pszIP,"0") || 0 == strcmp(pszIP,"0.0.0.0") 
		|| 0 == strcmp(pszIP,"*") 
	  )
	{
		nIP = htonl(INADDR_ANY);
	}
	else
	{
		nIP = inet_addr(pszIP);
	}
	addr.sin_addr.s_addr = nIP;

}

// 返回socket的fd
static int CreateTcpSocket(const unsigned short shPort /* = 0 */,const char *pszIP /* = "*" */,bool bReuse /* = false */)
{
	int fd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
	if( fd >= 0 )
	{
		if(shPort != 0)
		{
			if(bReuse)
			{
				int nReuseAddr = 1;
				setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&nReuseAddr,sizeof(nReuseAddr));
			}
			struct sockaddr_in addr ;
			SetAddr(pszIP,shPort,addr);
			int ret = bind(fd,(struct sockaddr*)&addr,sizeof(addr));
			if( ret != 0)
			{
				close(fd);
				return -1;
			}
		}
	}
	return fd;
}


int main(int argc,char *argv[])
{
	const char *ip = argv[1];
	int port = atoi( argv[2] );
	int cnt = atoi( argv[3] );  // 进程数量
	int proccnt = atoi( argv[4] );  // 每个进程的协程数量。（相当于协程池）。连接进来的客户端fd放在g_readwrite中，可能会比协程数量多。

	g_listen_fd = CreateTcpSocket( port,ip,true );
	listen( g_listen_fd,1024 );
	printf("listen %d %s:%d\n",g_listen_fd,ip,port);

	SetNonBlock( g_listen_fd );

	for(int k=0;k<proccnt;k++)
	{

		pid_t pid = fork();
		if( pid > 0 )
		{
			continue;
		}
		else if( pid < 0 )
		{
			break;
		}
		// 进入子进程

		for(int i=0;i<cnt;i++)  // 处理客户端请求的协程数量为cnt
		{
			task_t * task = (task_t*)calloc( 1,sizeof(task_t) );
			task->fd = -1;

			co_create( &(task->co),NULL,readwrite_routine,task );
			co_resume( task->co );  // 启动readwrite协程

		}
		stCoRoutine_t *accept_co = NULL;
		co_create( &accept_co,NULL,accept_routine,0 );
		co_resume( accept_co );  // 启动accept协程

		co_eventloop( co_get_epoll_ct(),0,0 );

		exit(0);
	}
	return 0;
}


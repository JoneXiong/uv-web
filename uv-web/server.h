#include "request.h"

int server_run(const char* hostaddr, const int port);
//bool server_init(const char* hostaddr, const int port);
//void server_run();
#ifdef WIN32
	#define GET_HANDLE_FD(hd) (((uv_tcp_t*)hd)->socket)
#else
	#define GET_HANDLE_FD(hd) hd->fd
#endif


#ifdef WIN32
	#define M_EWOULDBLOCK WSAEWOULDBLOCK
	#define M_SOMAXCONN SOMAXCONN
#else
	#define M_EWOULDBLOCK EWOULDBLOCK
	#define M_SOMAXCONN 4096
#endif

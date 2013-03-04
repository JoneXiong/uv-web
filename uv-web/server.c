//#include <sys/socket.h>
//#include <netinet/in.h>
//#include <arpa/inet.h>
#include <fcntl.h>
#ifdef WANT_SIGINT_HANDLING
# include <sys/signal.h>
#endif
//#include <sys/sendfile.h>
#include <uv.h>

#include "common.h"
#include "wsgi.h"
#include "server.h"

//释放对象
#define Py_XCLEAR(obj) do { if(obj) { Py_DECREF(obj); obj = NULL; } } while(0)
#define GIL_LOCK(n) _gilstate_##n = PyGILState_Ensure()	//线程状态控制
#define GIL_UNLOCK(n) PyGILState_Release(_gilstate_##n)
//libuv错误打印
#define UVERR(err, msg) fprintf(stderr, "%s: %s\n", msg, uv_strerror(err))

static const char* http_error_messages[4] = {
  NULL, /* Error codes start at 1 because 0 means "no error" */
  "HTTP/1.1 400 Bad Request\r\n\r\n",
  "HTTP/1.1 406 Length Required\r\n\r\n",
  "HTTP/1.1 500 Internal Server Error\r\n\r\n"
};

#define RESPONSE \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Type: text/plain\r\n" \
  "Content-Length: 12\r\n" \
  "\r\n" \
  "hello world\n"

#if WANT_SIGINT_HANDLING
typedef void ev_signal_callback(struct ev_loop*, ev_signal*, const int);
static ev_signal_callback ev_signal_on_sigint;
#endif

static void on_connection(uv_stream_t* server, int status);
static void on_read(uv_stream_t* handle, ssize_t nread, uv_buf_t buf);
static void on_close(uv_handle_t* handle); 


static bool send_chunk(Request*);
static bool do_sendfile(Request*);
static bool handle_nonzero_errno(Request*);
static void after_write(uv_write_t* req, int status);
static void io_write(Request* request);

static uv_tcp_t tcpServer;	//定义全局服务结构体
static uv_loop_t* loop;	//定义全局的事件循环

PyGILState_STATE _gilstate_0;

int server_run(const char* hostaddr, const int port)	//运行服务
{
  int r;
  struct sockaddr_in addr;

  loop = uv_default_loop();	
  addr = uv_ip4_addr(hostaddr, port);	//创建套接字地址
  r = uv_tcp_init(loop, &tcpServer);	//初始化服务handler创建套接字
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Socket creation error\n");
    return 1;
  }

  r = uv_tcp_bind(&tcpServer, addr);	//绑定套接字地址
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Bind error\n");
    return 1;
  }

  r = uv_listen((uv_stream_t*)&tcpServer, M_SOMAXCONN, on_connection);	//监听端口
  
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Listen error %s\n",
        uv_err_name(uv_last_error(loop)));
    return 1;
  }
  Py_BEGIN_ALLOW_THREADS
  uv_run(loop);
  Py_END_ALLOW_THREADS

#if WANT_SIGINT_HANDLING
//  ev_signal signal_watcher;
//  ev_signal_init(&signal_watcher, ev_signal_on_sigint, SIGINT);
//  ev_signal_start(mainloop, &signal_watcher);
#endif
  return 0;
}


uv_buf_t on_alloc(uv_handle_t* client, size_t suggested_size) {
  uv_buf_t buf;
  buf.base = malloc(suggested_size);
  buf.len = suggested_size;
  return buf;
}

static void on_connection(uv_stream_t* server, int status) {	//当有客户端连接时触发
	int r;
	Request* request;
	struct sockaddr_in sockaddr;	//定义套接字地址
	socklen_t addrlen;
	uv_stream_t* handle;
	dprint("on connection.");
	if (status != 0) {				//状态判断, 状态必须为0
	fprintf(stderr, "Connect error %d\n",
		uv_last_error(loop).code);
	}
	ASSERT(status == 0);

	handle = malloc(sizeof(uv_tcp_t));
	GIL_LOCK(0);
	request = Request_init();
	GIL_UNLOCK(0);

	r = uv_tcp_init(loop, (uv_tcp_t*)handle);//初始化连接流结构体
	ASSERT(r == 0);

	r = uv_accept(server, handle);
	ASSERT(r == 0);

	//-----------------得到客户端信息
	addrlen = sizeof(struct sockaddr_in);
	r = uv_tcp_getpeername((uv_tcp_t *)handle, (struct sockaddr *)&sockaddr, &addrlen);
	ASSERT(r == 0);
	
	request->client_addr = PyString_FromString(inet_ntoa(sockaddr.sin_addr));
	request->client_fd = 1;//request->ev_watcher.socket;
	request->ev_watcher = handle;
	handle->data = request;
	DBG_REQ(request, "Accepted client %s:%d on fd %d",
		  inet_ntoa(sockaddr.sin_addr), ntohs(sockaddr.sin_port), (int)(GET_HANDLE_FD(handle)));
	//@@@@@@@@@@@@@@@@@free(&sockaddr);
	//-----------------读事件循环
	r = uv_read_start(handle, on_alloc, on_read);
	ASSERT(r == 0);
}


#if WANT_SIGINT_HANDLING
static void
ev_signal_on_sigint(struct ev_loop* mainloop, ev_signal* watcher, const int events)
{
  /* Clean up and shut down this thread.
   * (Shuts down the Python interpreter if this is the main thread) */
  ev_unloop(mainloop, EVUNLOOP_ALL);
  PyErr_SetInterrupt();
}
#endif



static void after_write(uv_write_t* req, int status) {
  dprint("after_write");
  if (status) {
    uv_err_t err = uv_last_error(loop);
	UVERR(err,"uv_write error");
	//ASSERT(0);
  }
  free(req);
  //uv_close((uv_handle_t*)req->handle, on_close);
  /* Free the read/write buffer and the request */
  //@@@@@@@@@@@@@free(req);
}

static void on_read(uv_stream_t* handle, ssize_t nread, uv_buf_t buf) {
	Request* request;
	dprint("go on_read");
	request =(Request*)handle->data;
	GIL_LOCK(0);
	if (nread <= 0) {
		//@@@@@@@@@@@@@@@uv_err_t err = uv_last_error(loop);
		//@@@@@@@@@@@@@@@UVERR(err, "uv read error");
		if(nread == 0)
		        dprint("Client disconnected");
		uv_close((uv_handle_t*)handle, on_close);
		Request_free(request);
		if (buf.base) {
		  free(buf.base);
		}
		goto out;
	}
    Request_parse(request, buf.base, nread);	//处理Request解析
	free(buf.base);
    if(request->state.error_code) {
      DBG_REQ(request, "Parse error");
      request->current_chunk = PyString_FromString(http_error_messages[request->state.error_code]);//处理有错误的信息返回
	  assert(request->iterator == NULL);
	  //@@@@@@@@@ uv_close((uv_handle_t*) &request->ev_watcher, on_close);//解析有错误的请求直接关闭客户端连接
	  //@@@@@@@@@@ Request_free(request);
	  //@@@@@@@@@ goto out;
    }
    else if(request->state.parse_finished) {	// 解析好后
	  dprint("执行wsgi程序 >>>");
      if(!wsgi_call_application(request)) {	// 执行wsgi
	    dprint("wsgi执行包含错误");
        assert(PyErr_Occurred());
        PyErr_Print();	//打印python错误的跟踪堆栈信息
        assert(!request->state.chunked_response);
        Py_XCLEAR(request->iterator);
        request->current_chunk = PyString_FromString(	//返回错误信息给客户端
          http_error_messages[HTTP_SERVER_ERROR]);
      }
    } else {
      /* Wait for more data */
      goto out;
    }
    dprint("开始客户端数据返回 >>>");
    while(request->current_chunk){	//客户端返回循环
	  io_write(request);
    }

	out:
	  GIL_UNLOCK(0);
	  return;
}

static void on_close(uv_handle_t* handle) {
  free(handle);
  //dprint("on close.");
}

static void io_write(Request* request)
{
  //GIL_LOCK(0);

  if(request->state.use_sendfile) {
	dprint("发送文件给客户端");
    /* sendfile */
    if(request->current_chunk && send_chunk(request))
      goto out;
    /* abuse current_chunk_p to store the file fd */
    request->current_chunk_p = PyObject_AsFileDescriptor(request->iterable);
    if(do_sendfile(request))
      goto out;
  } else {
    dprint("发送字符");
    /* iterable */
	if(send_chunk(request)){
	  dprint("一次发送即完成");
	  //uv_close((uv_handle_t*) &request->ev_watcher, _http_uv__on_close__cb);
      goto out;
	}

    if(request->iterator) {
      PyObject* next_chunk;
	  dprint("request迭代");
      next_chunk = wsgi_iterable_get_next_chunk(request);
      if(next_chunk) {
		dprint("下一块chunk发送");
        if(request->state.chunked_response) {
          request->current_chunk = wrap_http_chunk_cruft_around(next_chunk);
          Py_DECREF(next_chunk);
        } else {
          request->current_chunk = next_chunk;
        }
        assert(request->current_chunk_p == 0);
		//io_write(request);
        goto out;
      } else {
        if(PyErr_Occurred()) {
		  uv_err_t err;
		  dprint("迭代出错");
          PyErr_Print();
          DBG_REQ(request, "Exception in iterator, can not recover");
		  uv_close((uv_handle_t*) request->ev_watcher, on_close);
		  Request_free(request);
		  err = uv_last_error(loop);
		  UVERR(err, "uv_write error on next chunk");
		  ASSERT(0);
          goto out;
        }
		dprint("没有下一块chunk");
        Py_CLEAR(request->iterator);
      }
    }

    if(request->state.chunked_response) {
      dprint("如果是chunked_response 发送收尾数据,并置空chunked_response");
      /* We have to send a terminating empty chunk + \r\n */
      request->current_chunk = PyString_FromString("0\r\n\r\n");
      assert(request->current_chunk_p == 0);
	  //io_write(request);
      request->state.chunked_response = false;
      goto out;
    }
  }
  dprint("响应完成");
  if(request->state.keep_alive) {
    DBG_REQ(request, "done, keep-alive");
    Request_clean(request);
    Request_reset(request);
  } else {
	dprint("done not keep alive");
	uv_close((uv_handle_t*) request->ev_watcher, on_close);
	Request_free(request);
  }

out:
  dprint("本次字符发送结束");
  //GIL_UNLOCK(0);
  return;
}

static bool
send_chunk(Request* request)
{
  Py_ssize_t bytes_sent;
  static uv_buf_t resbuf;
  uv_write_t * wr;
  wr = (uv_buf_t*) malloc(sizeof *wr);
  //dprint("发送chunk:\n%s",PyString_AS_STRING(request->current_chunk) + request->current_chunk_p);
  dprint("发送大小:%d",PyString_GET_SIZE(request->current_chunk) - request->current_chunk_p);
  assert(request->current_chunk != NULL);
  assert(!(request->current_chunk_p == PyString_GET_SIZE(request->current_chunk)
         && PyString_GET_SIZE(request->current_chunk) != 0));
  resbuf = uv_buf_init(PyString_AS_STRING(request->current_chunk) + request->current_chunk_p, PyString_GET_SIZE(request->current_chunk) - request->current_chunk_p);
  bytes_sent = uv_write(
		   wr,
		   request->ev_watcher,
		   &resbuf,
		   1,
		   after_write);

  if(bytes_sent == -1){
    dprint("发送数据出错");
	dprint("chunk:\n%s",PyString_AS_STRING(request->current_chunk) + request->current_chunk_p);
    return handle_nonzero_errno(request);
  }
  request->current_chunk_p += resbuf.len;
  if(request->current_chunk_p == PyString_GET_SIZE(request->current_chunk)) {
    Py_CLEAR(request->current_chunk);
    request->current_chunk_p = 0;
    return false;
  }
  //@@@@@@@@@@@@@@@@@ Py_CLEAR(request->current_chunk);
  //@@@@@@@@@@@@@@@@@ free(resbuf.base);
  return true;
}

#define SENDFILE_CHUNK_SIZE 16*1024

static bool
do_sendfile(Request* request)
{
  Py_ssize_t bytes_sent = 1;
	 // sendfile(
  //  request->client_fd,
  //  request->current_chunk_p, /* current_chunk_p stores the file fd */
  //  NULL, SENDFILE_CHUNK_SIZE
  //);
  if(bytes_sent == -1)
    return handle_nonzero_errno(request);
  return bytes_sent != 0;
}

static bool
handle_nonzero_errno(Request* request)
{
  if(errno == EAGAIN || errno == M_EWOULDBLOCK) {	//WSAEWOULDBLOCK
    /* Try again later */
    return true;
  } else {
    /* Serious transmission failure. Hang up. */
    fprintf(stderr, "Client %d hit errno %d\n", request->client_fd, errno);
    Py_XDECREF(request->current_chunk);
    Py_XCLEAR(request->iterator);
    request->state.keep_alive = false;
    Request_clean(request);
    Request_reset(request);
    return false;
  }
}



#ifndef __request_h__
#define __request_h__

#include <uv.h>
#include "http_parser.h"
#include "common.h"

void _initialize_request_module(const char* host, const int port);

typedef struct {
  unsigned error_code : 2;
  unsigned parse_finished : 1;
  unsigned start_response_called : 1;
  unsigned wsgi_call_done : 1;
  unsigned keep_alive : 1;
  unsigned response_length_unknown : 1;
  unsigned chunked_response : 1;
  unsigned use_sendfile : 1;
} request_state;	// ��������״̬�ֵ�

typedef struct {
  http_parser parser;	//������
  string field;	//�ֶ�
  string value;	//ֵ
  string body;	 //body��
} bj_parser;

typedef struct {	//Request����Ķ���
#ifdef DEBUG
  unsigned long id;
#endif
  bj_parser parser;
  uv_stream_t* ev_watcher;
  uv_write_t write_req;
  int client_fd;	//�ͻ����׽���ID
  PyObject* client_addr;	//�ͻ����׽��ֵ�ַ

  request_state state;	//����״̬

  PyObject* status;	//״̬��
  PyObject* headers;	//����ͷ
  PyObject* current_chunk;
  Py_ssize_t current_chunk_p;
  PyObject* iterable;	//�ɵ���
  PyObject* iterator;	//������
} Request;

#define REQUEST_FROM_WATCHER(watcher) \
  (Request*)((size_t)watcher - (size_t)(&(((Request*)NULL)->ev_watcher)));

Request* Request_new(int client_fd, const char* client_addr);
Request* Request_init();
void Request_parse(Request*, const char*, const size_t);
void Request_reset(Request*);
void Request_clean(Request*);
void Request_free(Request*);

#endif


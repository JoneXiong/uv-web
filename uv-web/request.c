#define inline __inline

#include <Python.h>
#include <cStringIO.h>
#include "request.h"
#include "filewrapper.h"

static inline void PyDict_ReplaceKey(PyObject* dict, PyObject* k1, PyObject* k2);
static PyObject* wsgi_http_header(string header);
static http_parser_settings parser_settings;
static PyObject* wsgi_base_dict = NULL;	//wsgi������������Ϣ�ֵ�				ȫ�ֱ���

/* Non-public type from cStringIO I abuse in on_body */
typedef struct {
  PyObject_HEAD
  char *buf;
  Py_ssize_t pos, string_size;
  PyObject *pbuf;
} Iobject;

Request* Request_new(int client_fd, const char* client_addr)
{
  Request* request = malloc(sizeof(Request));	//�����ڴ�
#ifdef DEBUG
  static unsigned long request_id = 0;
  request->id = request_id++;
#endif
  request->client_fd = client_fd;
  request->client_addr = PyString_FromString(client_addr);
  http_parser_init((http_parser*)&request->parser, HTTP_REQUEST);	//��ʼ������ṹ,�����úûص�����.    HTTP Parser��һ����C��д��HTTP��Ϣ��������. �ȷ�������Ҳ������Ӧ������ʹ���κ�ϵͳ����Ҳ�������ڴ棬����������, �������κ�ʱ���ж�
  request->parser.parser.data = request;
  Request_reset(request);
  return request;
}

Request* Request_init()
{
  Request* request = malloc(sizeof(Request));	//�����ڴ�
#ifdef DEBUG
  static unsigned long request_id = 0;
  request->id = request_id++;
#endif
  http_parser_init((http_parser*)&request->parser, HTTP_REQUEST);	//��ʼ������ṹ,�����úûص�����.    HTTP Parser��һ����C��д��HTTP��Ϣ��������. �ȷ�������Ҳ������Ӧ������ʹ���κ�ϵͳ����Ҳ�������ڴ棬����������, �������κ�ʱ���ж�
  request->parser.parser.data = request;
  Request_reset(request);
  return request;
}

void Request_reset(Request* request)	//Request����ĳ�ʼ�� state �� parser.body���
{
  string m;
  dprint("Request_reset...");
  memset(&request->state, 0, sizeof(Request) - (size_t)&((Request*)NULL)->state);	//memset:��������һ���ڴ�������ĳ��������ֵ�����Խϴ�Ľṹ�������������������һ����췽��
  request->state.response_length_unknown = true;
  m.data = NULL;
  m.len = 0;
  request->parser.body = m;//(string){NULL, 0};
}

void Request_free(Request* request)
{
  dprint("Request_free...");
  Request_clean(request);
  Py_DECREF(request->client_addr);
  free(request);
}

void Request_clean(Request* request)
{
  dprint("Request_clean...");
  if(request->iterable) {
    /* Call 'iterable.close()' if available */
    PyObject* close_method = PyObject_GetAttr(request->iterable, _close_m);
    if(close_method == NULL) {
      if(PyErr_ExceptionMatches(PyExc_AttributeError))
        PyErr_Clear();
    } else {
      PyObject_CallObject(close_method, NULL);
      Py_DECREF(close_method);
    }
    if(PyErr_Occurred()) PyErr_Print();
    Py_DECREF(request->iterable);
  }
  Py_XDECREF(request->iterator);
  Py_XDECREF(request->headers);
  Py_XDECREF(request->status);
}

/* Parse stuff */

void Request_parse(Request* request, const char* data, const size_t data_len)
{
  size_t nparsed;
  assert(data_len);
  dprint("������������:%d\n%s",data_len,data);
  nparsed = http_parser_execute((http_parser*)&request->parser,
                                       &parser_settings, data, data_len);
  dprint("������ɵ�����:%d",nparsed);
  if(nparsed != data_len)
  {
    dprint("************HTTP_BAD_REQUEST************ %d | %d \n",data_len,nparsed);
    request->state.error_code = HTTP_BAD_REQUEST;
  }
}

#define REQUEST ((Request*)parser->data)
#define PARSER  ((bj_parser*)parser)
#define UPDATE_LENGTH(name) \
  /* Update the len of a header field/value.
   *
   * Short explaination of the pointer arithmetics fun used here:
   *
   *   [old header data ] ...stuff... [ new header data ]
   *   ^-------------- A -------------^--------B--------^
   *
   * A = XXX- PARSER->XXX.data
   * B = len
   * A + B = old header start to new header end
   */ \
  do { PARSER->name.len = (name - PARSER->name.data) + len; } while(0)

#define _set_header(k, v) PyDict_SetItem(REQUEST->headers, k, v);
#define _set_header_free_value(k, v) \
  do { \
    PyObject* val = (v); \
	dprint("_set_header_free_value\n");\
	_set_header(k, val); \
    Py_DECREF(val); \
  } while(0)
#define _set_header_free_both(k, v) \
  do { \
    PyObject* key = (k); \
    PyObject* val = (v); \
    _set_header(key, val); \
    Py_DECREF(key); \
    Py_DECREF(val); \
  } while(0)

static int on_message_begin(http_parser* parser)
{
  string s1 = {NULL,0};
  string s2 = {NULL,0};
  REQUEST->headers = PyDict_New();
  PARSER->field = s1;
  PARSER->value = s2;
  return 0;
}

static int on_path(http_parser* parser, char* path, size_t len)
{
  dprint("on_path");
  if(!(len = unquote_url_inplace(path, len)))
    return 1;
  _set_header_free_value(_PATH_INFO, PyString_FromStringAndSize(path, len));
  return 0;
}

static int on_query_string(http_parser* parser, const char* query, size_t len)
{
  dprint("on_query_string");
  _set_header_free_value(_QUERY_STRING, PyString_FromStringAndSize(query, len));
  return 0;
}

static int on_header_field(http_parser* parser, const char* field, size_t len)
{
  string s1;
  string s2={NULL, 0};
  if(PARSER->value.data) {
    /* Store previous header and start a new one */
    _set_header_free_both(
      wsgi_http_header(PARSER->field),
      PyString_FromStringAndSize(PARSER->value.data, PARSER->value.len)
    );
  } else if(PARSER->field.data) {
    UPDATE_LENGTH(field);
    return 0;
  }
  s1.data = (char*)field;
  s1.len = len;
  PARSER->field = s1;
  PARSER->value = s2;
  return 0;
}

static int
on_header_value(http_parser* parser, const char* value, size_t len)
{
  if(PARSER->value.data) {
    UPDATE_LENGTH(value);
  } else {
    /* Start a new value */
	string s;
	s.data = (char*)value;
	s.len = len;
    PARSER->value = s;
  }
  return 0;
}




static int on_headers_complete(http_parser* parser)
{
  dprint("on_headers_complete");
  if(PARSER->field.data) {
    _set_header_free_both(
      wsgi_http_header(PARSER->field),
      PyString_FromStringAndSize(PARSER->value.data, PARSER->value.len)
    );
  }
  return 0;
}

static int
on_body(http_parser* parser, const char* data, const size_t len)
{
  Iobject* body;
  dprint("on_body");
  body = (Iobject*)PyDict_GetItem(REQUEST->headers, _wsgi_input);
  if(body == NULL) {
    PyObject* buf;
    if(!parser->content_length) {
      REQUEST->state.error_code = HTTP_LENGTH_REQUIRED;
      return 1;
    }
    buf = PyString_FromStringAndSize(NULL, parser->content_length);
    body = (Iobject*)PycStringIO->NewInput(buf);
    Py_XDECREF(buf);
    if(body == NULL)
      return 1;
    _set_header(_wsgi_input, (PyObject*)body);
    Py_DECREF(body);
  }
  memcpy(body->buf + body->pos, data, len);
  body->pos += len;
  return 0;
}

static int
on_message_complete(http_parser* parser)
{
  PyObject* body;
  dprint("on_message_complete");
  /* HTTP_CONTENT_{LENGTH,TYPE} -> CONTENT_{LENGTH,TYPE} */
  PyDict_ReplaceKey(REQUEST->headers, _HTTP_CONTENT_LENGTH, _CONTENT_LENGTH);
  PyDict_ReplaceKey(REQUEST->headers, _HTTP_CONTENT_TYPE, _CONTENT_TYPE);

  /* SERVER_PROTOCOL (REQUEST_PROTOCOL) */
  _set_header(_SERVER_PROTOCOL, parser->http_minor == 1 ? _HTTP_1_1 : _HTTP_1_0);

  /* REQUEST_METHOD */
  if(parser->method == HTTP_GET) {
    /* I love useless micro-optimizations. */
    _set_header(_REQUEST_METHOD, _GET);
  } else {
    _set_header_free_value(_REQUEST_METHOD,
      PyString_FromString(http_method_str(parser->method)));
  }

  /* REMOTE_ADDR */
  _set_header(_REMOTE_ADDR, REQUEST->client_addr);

  body = PyDict_GetItem(REQUEST->headers, _wsgi_input);
  if(body) {
    /* We abused the `pos` member for tracking the amount of data copied from
     * the buffer in on_body, so reset it to zero here. */
    ((Iobject*)body)->pos = 0;
  } else {
    /* Request has no body */
    _set_header_free_value(_wsgi_input, PycStringIO->NewInput(_empty_string));
  }

  PyDict_Update(REQUEST->headers, wsgi_base_dict);

  REQUEST->state.parse_finished = true;
  return 0;
}


static PyObject*
wsgi_http_header(string header)
{
  PyObject* obj = PyString_FromStringAndSize(NULL, header.len+strlen("HTTP_"));
  char* dest = PyString_AS_STRING(obj);

  *dest++ = 'H';
  *dest++ = 'T';
  *dest++ = 'T';
  *dest++ = 'P';
  *dest++ = '_';

  while(header.len--) {
    char c = *header.data++;
    if(c == '-')
      *dest++ = '_';
    else if(c >= 'a' && c <= 'z')
      *dest++ = c - ('a'-'A');
    else
      *dest++ = c;
  }

  return obj;
}

static inline void
PyDict_ReplaceKey(PyObject* dict, PyObject* old_key, PyObject* new_key)
{
  PyObject* value = PyDict_GetItem(dict, old_key);
  if(value) {
    Py_INCREF(value);
    PyDict_DelItem(dict, old_key);
    PyDict_SetItem(dict, new_key, value);
    Py_DECREF(value);
  }
}

// http�������ص���������
static http_parser_settings
parser_settings = {
  on_message_begin, on_path, on_query_string, NULL, NULL, on_header_field,
  on_header_value, on_headers_complete, on_body, on_message_complete
};

// wsgi�����������Ϣ��ʼ��
void _initialize_request_module(const char* server_host, const int server_port)	
{
  if(wsgi_base_dict == NULL) {	//C�����У����е�Python���Ͷ�������ΪPyObject�� Py_BuildValue()���������ֺ��ַ�������ת������ʹ֮���Python����Ӧ���������� PyObject* Py_BuildValue( const char *format, ...)
    PycString_IMPORT;
    wsgi_base_dict = PyDict_New();	//����python�ֵ�

    /* dct['wsgi.file_wrapper'] = FileWrapper */
    PyDict_SetItemString(
      wsgi_base_dict,
      "wsgi.file_wrapper",
      (PyObject*)&FileWrapper_Type
    );

    /* dct['SCRIPT_NAME'] = '' */	//�������������� SCRIPT_NAME Ĭ��Ϊ��
    PyDict_SetItemString(
      wsgi_base_dict,
      "SCRIPT_NAME",
      _empty_string		// python������
    );

    /* dct['wsgi.version'] = (1, 0) */	//�������汾
    PyDict_SetItemString(
      wsgi_base_dict,
      "wsgi.version",
      PyTuple_Pack(2, PyInt_FromLong(1), PyInt_FromLong(0))	// pythonԪ��
    );

    /* dct['wsgi.url_scheme'] = 'http'	//����Э������ 	��ȫ�����Э�飨TLS��
     * (This can be hard-coded as there is no TLS support in uvweb.) */
    PyDict_SetItemString(
      wsgi_base_dict,
      "wsgi.url_scheme",
      PyString_FromString("http")	// python�ַ�������
    );

    /* dct['wsgi.errors'] = sys.stderr */	// ϵͳ��׼�����豸
    PyDict_SetItemString(
      wsgi_base_dict,
      "wsgi.errors",
      PySys_GetObject("stderr")	// python sysģ���׼�����豸
    );

    /* dct['wsgi.multithread'] = True
     * If I correctly interpret the WSGI specs, this means
     * "Can the server be ran in a thread?" */
    PyDict_SetItemString(	// �Ƿ���߳�
      wsgi_base_dict,
      "wsgi.multithread",
      Py_True	// python ��������
    );

    /* dct['wsgi.multiprocess'] = True
     * ... and this one "Can the server process be forked?" */
    PyDict_SetItemString(	// �Ƿ�����
      wsgi_base_dict,
      "wsgi.multiprocess",
      Py_True
    );

    /* dct['wsgi.run_once'] = False (uvweb is no CGI gateway) */
    PyDict_SetItemString(	// �Ƿ� CGIʽ��run_onceģʽ
      wsgi_base_dict,
      "wsgi.run_once",
      Py_False
    );
  }

  PyDict_SetItemString(	//��������IP
    wsgi_base_dict,
    "SERVER_NAME",
    PyString_FromString(server_host)
  );

  PyDict_SetItemString(	//�˿ں�
    wsgi_base_dict,
    "SERVER_PORT",
    PyString_FromFormat("%d", server_port)
  );
}



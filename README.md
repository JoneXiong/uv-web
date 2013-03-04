uv-web
======

A  wsgi web server for python using libuv

概述
-------

uv-web是一个轻量级的支持高并发的WSGI Web服务器，基于libuv构建，本质是python的C扩展，所以适用于部署绝大部分
python web应用(如 Django)

特性
------------

  1. 兼容 HTTP 1.0/1.1
  2. 支持 keep-alive
  3. 基于libuv事件循环库，跨平台性良好，并发表现不错
  4. 部署方便，相当于python扩展模块


安装
-----------
::
  linux/unix系统下：
  1.下载源码
  2. python setup.py install
::
  Windows下：
  1.下载源码，找到vc工程文件uv-web.sn(vs2008)，打开vc工程(在/vc-proj目录下)
  2.设置好libuv和python的 include目录和lib目录(可参考 [博客](http://www.cnblogs.com/johan/archive/2013/02/27/2935688.html) )
  3.编译(参数DEBUG_DEV和DEBUG可用于控制一些打印调试信息)

使用
-----
::
一般运行方式
import uvweb
bjoern.run(wsgi_application, host, port)

Django web 部署示例：
import uvweb
import django.core.handlers.wsgi
uvweb.listens(django.core.handlers.wsgi.WSGIHandler(), '0.0.0.0', 8080)


测试
-------
在一般配置pc上，局域网环境下用ab工具做过一个简单的echo web程序测试
windows下
Requests per second:    9655.44 [#/sec] 

Ubuntu linux下
Requests per second:    5216.36 [#/sec] 

  

# ngx_stream_app_module
基于ngx_stream_module和ngx_thread_pool_module的通用TCP服务器@nginx/1.9.4

nginx.conf配置：

---------------------------------------
thread_pool app threads=10;

stream {
    server {
        listen 10000;
        app;
        res_buf_size 10240;
        header_len 6;#报文头长度
        send_timeout 30;
        client_timeout 30;         
    }
}
---------------------------------------
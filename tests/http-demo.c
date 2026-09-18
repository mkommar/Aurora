/* Build inside Aurora: gcc -static http-demo.c -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -o http-demo */
#include <curl/curl.h>
#include <stdio.h>
int main(int argc,char **argv){
    if(curl_global_init(CURL_GLOBAL_DEFAULT))return 1;
    CURL *client=curl_easy_init();if(!client){curl_global_cleanup();return 1;}
    curl_easy_setopt(client,CURLOPT_URL,argc>1?argv[1]:"https://example.com/");
    curl_easy_setopt(client,CURLOPT_PROTOCOLS_STR,"http,https");
    curl_easy_setopt(client,CURLOPT_TIMEOUT,30L);
    curl_easy_setopt(client,CURLOPT_FAILONERROR,1L);
    CURLcode result=curl_easy_perform(client);
    if(result)fprintf(stderr,"HTTP request: %s\n",curl_easy_strerror(result));
    curl_easy_cleanup(client);curl_global_cleanup();return result?1:0;
}

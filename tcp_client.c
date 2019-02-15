#include <stdio.h> /* printf, sprintf */
#include <stdlib.h> /* exit, atoi, malloc, free */
#include <unistd.h> /* read, write, close */
#include <string.h> /* memcpy, memset */
#include <sys/socket.h> /* socket, connect */
#include <netinet/in.h> /* struct sockaddr_in, struct sockaddr */
#include <netdb.h> /* struct hostent, gethostbyname */

void error(const char *msg) { perror(msg); exit(0); }

// post_request("192.168.0.104", "1337", "/api/pluto", "{\"pid\": \"2\", \"RSS\": \"-56 dB\"}");

int post_request(char* ip, char* port, char* endpoint, char* data){
    printf("in post func\n");
    char *message, response[4096];
    printf("port\n");
    int portno = atoi(port);
    printf("port\n");
    printf("port %d\n", portno);
    char *host = ip;
    printf("host %s\n", host);
    struct hostent *server;
    struct sockaddr_in serv_addr;
    int sockfd, bytes, sent, received, total, message_size;

    message_size = 0;
    printf("size of message = %d\n", message_size);
    message_size+=strlen("%s %s HTTP/1.0\r\n");
    printf("size of message = %d\n", message_size);
    message_size+=strlen("POST");                         /* method         */
    printf("size of message = %d\n", message_size);
    message_size+=strlen(endpoint);                         /* path           */
    printf("size of message = %d\n", message_size);
    message_size+=strlen("Content-Type: application/json")+strlen("\r\n");
    printf("size of message = %d\n", message_size);
    message_size+=strlen("Content-Length: %d\r\n")+10; /* content length */
    printf("size of message = %d\n", message_size);
    message_size+=strlen("\r\n");                          /* blank line     */
    printf("size of message = %d\n", message_size);
    printf("i am here\n");
    printf("%s \n", data);
    message_size+=strlen(data);
    printf("size of message = %d\n", message_size);   

    printf("size of message = %d\n", message_size);
    message=malloc(message_size);

    sprintf(message,"%s %s HTTP/1.0\r\n","POST",endpoint);
    strcat(message,"Content-Type: application/json");strcat(message,"\r\n");
    sprintf(message+strlen(message),"Content-Length: %lu\r\n",strlen(data));    
    strcat(message,"\r\n");
    strcat(message,data);

    /* What are we going to send? */
    printf("Request:\n%s\n\n",message);

    /* create the socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");

    /* lookup the ip address */
    server = gethostbyname(host);
    if (server == NULL) error("ERROR, no such host");

    /* fill in the structure */
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    memcpy(&serv_addr.sin_addr.s_addr,server->h_addr,server->h_length);

    /* connect the socket */
    if (connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr)) < 0)
        error("ERROR connecting");

    /* send the request */
    total = strlen(message);
    sent = 0;
    do {
        bytes = write(sockfd,message+sent,total-sent);
        if (bytes < 0)
            error("ERROR writing message to socket");
        if (bytes == 0)
            break;
        sent+=bytes;
    } while (sent < total);

    /* receive the response */
    memset(response,0,sizeof(response));
    total = sizeof(response)-1;
    received = 0;
    do {
        bytes = read(sockfd,response+received,total-received);
        if (bytes < 0)
            error("ERROR reading response from socket");
        if (bytes == 0)
            break;
        received+=bytes;
    } while (received < total);

    if (received == total)
        error("ERROR storing complete response from socket");

    /* close the socket */
    close(sockfd);

    /* process response */
    printf("Response:\n%s\n",response);

    free(message);
    return 0;
}
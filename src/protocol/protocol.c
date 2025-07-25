#define MAXLEN 32
struct custom_protocol
{
  int clientfd;
  char req[MAXLEN];
};

// void parse(int clientfd, int buf, int nbytes)
// {
//    
// }

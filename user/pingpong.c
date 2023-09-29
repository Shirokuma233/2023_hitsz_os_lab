#include "kernel/types.h"
#include "user.h"

int main(int argc, char *argv[])
{
  int p1[2], p2[2];
  pipe(p1);
  pipe(p2);
  int pid = fork();
  if(pid == 0) //子进程
  {
    char child_rev[5];
    close(p1[1]);
    read(p1[0], child_rev, 5);
    close(p1[0]);
    printf("%d: received ping\n", getpid());
    close(p2[0]);
    write(p2[1], "pong", 5);
    close(p2[1]);
  }
  else  //父进程
  {
    char father_rev[5];
    close(p1[0]);
    write(p1[1], "ping", 5);
    close(p1[1]);
    close(p2[1]);
    read(p2[0], father_rev, 5);
    close(p2[0]);
    printf("%d: received pong\n", getpid());
  }
  exit(0);
}

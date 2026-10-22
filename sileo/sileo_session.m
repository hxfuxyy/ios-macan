#import <Foundation/Foundation.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
static void session_log(NSString *format, ...) {
 va_list args;va_start(args,format);NSString *text=[[NSString alloc] initWithFormat:format arguments:args];va_end(args);
 fprintf(stderr,"%s\n",[text UTF8String]);fflush(stderr);NSLog(@"%@",text);
}
static int run(char *const args[]) {
 pid_t pid=fork();if(pid==0){execv(args[0],args);_exit(127);}
 if(pid<0){session_log(@"Sileo-test: fork errno=%d",errno);return -1;}
 int status;while(waitpid(pid,&status,0)<0){if(errno!=EINTR)return -1;}
 session_log(@"Sileo-test: %s exit=%d signal=%d",args[0],WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);
 return WIFEXITED(status)?WEXITSTATUS(status):-1;
}
int main(void){@autoreleasepool{
 if(access("/var/jb/Applications/Sileo.app/Info.plist",R_OK)!=0) {char *copy[]={"/redwood/bin/bootstrap-copy",NULL};if(run(copy))return 1;}
 session_log(@"Sileo-test: session started pid=%d",getpid());
 NSString *path=@"/var/jb/Applications/Sileo.app/Info.plist";
 NSDictionary *info=[NSDictionary dictionaryWithContentsOfFile:path];
 session_log(@"Sileo-test: installed bundle=%@, dpkg exists=%d",info[@"CFBundleIdentifier"],access("/var/jb/usr/bin/dpkg",X_OK)==0);
 if(!info)return 1;
 setenv("NO_PASSWORD_PROMPT","1",1);setenv("PATH","/var/jb/usr/bin:/var/jb/usr/sbin:/var/jb/bin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin",1);
 char *prep[]={"/var/jb/bin/bash","/var/jb/prep_bootstrap.sh",NULL};run(prep);
 char *dpkg[]={"/var/jb/usr/bin/dpkg","--version",NULL};run(dpkg);
 session_log(@"Sileo-test: system app present=%d",access("/Applications/Sileo.app/Sileo",X_OK)==0);
 return 0;
}}

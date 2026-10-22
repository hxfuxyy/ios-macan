#include "mini.h"
static char input[8192], line[4096], bytes[16384];
static size_t at, used;
static int nextline(int fd) {
    size_t n=0;
    for (;;) {
        if (at==used) { ssize_t r=read(fd,input,sizeof(input)); if(r<0)return -1; if(!r)return n ? -1:0; used=(size_t)r; at=0; }
        char c=input[at++];
        if(c=='\n'){line[n]=0;return 1;}
        if(n+1>=sizeof(line))return -1;
        line[n++]=c;
    }
}
static unsigned number(const char *s,unsigned base) {unsigned v=0; for(;*s;s++){if(*s<'0'||*s>='0'+base)return 0;v=v*base+*s-'0';}return v;}
static int copy(const char *src,const char *dst,unsigned mode) {
    int in=open(src,O_RDONLY,0); if(in<0)return 0;
    int out=open(dst,O_CREAT|O_WRONLY|O_TRUNC,mode);if(out<0){close(in);return 0;}
    ssize_t n; int ok=1;
    while((n=read(in,bytes,sizeof(bytes)))>0) {ssize_t sent=0;while(sent<n){ssize_t r=write(out,bytes+sent,(size_t)(n-sent));if(r<=0){ok=0;break;}sent+=r;}if(!ok)break;}
    close(in);close(out);return ok&&n==0;
}
int main(int argc,char **argv,char **envp) {
    (void)argc;(void)argv;(void)envp;
    int fd=open("/redwood/bootstrap/manifest",O_RDONLY,0);if(fd<0){printf("bootstrap: no manifest\n");return 1;}
    int status;unsigned count=0, failed=0;
    while((status=nextline(fd))>0) {
        char *field[6]={line};int fields=1;
        for(char *p=line;*p;p++)if(*p=='\t'){*p=0;if(fields==6){failed++;break;}field[fields++]=p+1;}
        if(fields<5||strncmp(field[4],"/private/var/jb",15)|| (field[4][15]&&field[4][15]!='/')){failed++;continue;}
        unsigned mode=number(field[1],8);int ok=0;
        if(field[0][0]=='d') {struct stat st;ok=mkdir(field[4],mode)==0 || (lstat(field[4],&st)==0&&(st.st_mode&S_IFMT)==S_IFDIR);}
        else if(field[0][0]=='f'&&fields==6) ok=copy(field[5],field[4],mode);
        else if(field[0][0]=='l'&&fields==6) {unlink(field[4]);ok=symlink(field[5],field[4])==0;}
        if(ok&&field[0][0]!='l')ok=chown(field[4],number(field[2],10),number(field[3],10))==0&&chmod(field[4],mode)==0;
        if(!ok){if(failed<15)printf("bootstrap: failed %s errno %d\n",field[4],errno);failed++;}
        count++;
    }
    close(fd);printf("bootstrap: %u entries, %u failures\n",count,failed);
    return failed||status<0 ? 1:0;
}

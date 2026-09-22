#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void){
    char path[128];struct stat st;int pinned=-1;
    for(int i=0;i<20000;i++){
        snprintf(path,sizeof(path),"/work/cache-fixture/d%03d/f%03d",i/100,i%100);
        assert(!stat(path,&st) && st.st_size==0);
        if(i==12000){pinned=open(path,O_RDONLY);assert(pinned>=0);}
        if(i%2000==0){printf("CACHE_PROGRESS %d\n",i);fflush(stdout);}
    }
    assert(pinned>=0 && !fstat(pinned,&st) && st.st_size==0);
    char byte;assert(read(pinned,&byte,1)==0);assert(!close(pinned));
    for(int i=0;i<20000;i+=97){
        snprintf(path,sizeof(path),"/work/cache-fixture/d%03d/f%03d",i/100,i%100);
        assert(!stat(path,&st) && st.st_size==0);
    }
    puts("AURORA_PATH_CACHE_PASS");return 0;
}

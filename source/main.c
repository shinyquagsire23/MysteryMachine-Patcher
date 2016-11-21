#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "decomp.h"
#include "common.h"

u32 _firm_appmemalloc = 0x04000000;

paramblk_t* paramblk = NULL;

void buildMap(memorymap_t* mm, u32 size)
{
    if(!mm)return;

    // init
    mm->num = 0;

    // figure out the physical memory map
    u32 current_size = size;
    u32 current_offset = 0x00000000;

    int i;
    for(i=0; i<3; i++)
    {
        const u32 mask = 0x000fffff >> (4*i);
        u32 section_size = current_size & ~mask;
        if(section_size)
        {
            if(!mm->num) mm->processLinearOffset = section_size;
            current_offset += section_size;
            mm->map[mm->num++] = (memorymap_entry_t){0x00100000 + current_offset - section_size, mm->processLinearOffset - current_offset, section_size};
            printf("%d : %08X - %08X\n", mm->num-1, (unsigned int)mm->map[mm->num-1].dst, (unsigned int)(mm->map[mm->num-1].dst + mm->map[mm->num-1].size));
            current_size -= section_size;
        }
    }
}

memorymap_t processMap;

// bypass handle list
Result _srvGetServiceHandle(Handle* out, const char* name)
{
    Result rc = 0;

    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = 0x50100;
    strcpy((char*) &cmdbuf[1], name);
    cmdbuf[3] = strlen(name);
    cmdbuf[4] = 0x0;
    
    if((rc = svcSendSyncRequest(*srvGetSessionHandle())))return rc;

    *out = cmdbuf[3];
    return cmdbuf[1];
}

Result getTitleInformation(u8* mediatype, u64* tid)
{
    Result ret = 0;

    if(mediatype)
    {
        Handle localFsHandle;

        ret = srvGetServiceHandleDirect(&localFsHandle, "fs:USER");
        if(ret)return ret;
        
        ret = FSUSER_Initialize(localFsHandle);
        if(ret)return ret;
        fsUseSession(localFsHandle, false);

        ret = FSUSER_GetMediaType(mediatype);

        fsEndUseSession();
        svcCloseHandle(localFsHandle);
    }

    if(tid)
    {
        aptOpenSession();
        ret = APT_GetProgramID(tid);
        aptCloseSession();
    }

    return ret;
}

Result openCode(Handle* out, u64 tid, u8 mediatype)
{
    if(!out)return -1;
    
    Result ret = 0;

    u32 archivePath[] = {tid & 0xFFFFFFFF, (tid >> 32) & 0xFFFFFFFF, mediatype, 0x00000000};
    static const u32 filePath[] = {0x00000000, 0x00000000, 0x00000002, 0x646F632E, 0x00000065};

    ret = FSUSER_OpenFileDirectly(out, (FS_Archive){0x2345678a, (FS_Path){PATH_BINARY, 0x10, (u8*)archivePath}}, (FS_Path){PATH_BINARY, 0x14, (u8*)filePath}, FS_OPEN_READ, 0);
    return ret;
}

u32 search_string(char *string, u32 start, u32 len, u8 *data)
{
    for(int i = start; i < start+len; i++)
    {
        if(strcmp(string, data+i) == 0)
            return i;
    }
    return -1;
}

Result loadCode()
{
    Result ret;
    Handle fileHandle;

    u8 mediatype = 0;
    u64 tid = 0;

    ret = getTitleInformation(&mediatype, &tid);

    printf("%08X : %d, %08X, %08X\n", (unsigned int)ret, mediatype, (unsigned int)tid & 0xFFFFFFFF, (unsigned int)(tid >> 32) & 0xFFFFFFFF);

    // // if we supported updates we'd do this
    // ret = openCode(&fileHandle, tid | 0x0000000E00000000LL, 1);
    // if(ret) ret = openCode(&fileHandle, tid, mediatype);

    // but right now we don't so too bad
    ret = openCode(&fileHandle, tid, mediatype);

    printf("loading code : %08X\n", (unsigned int)ret);

    u8* fileBuffer = NULL;
    u64 fileSize = 0;

    if(!ret)
    {
        u32 bytesRead;

        ret = FSFILE_GetSize(fileHandle, &fileSize);

        fileBuffer = malloc(fileSize);

        ret = FSFILE_Read(fileHandle, &bytesRead, 0x0, fileBuffer, fileSize);
        if(ret)
        {
            free(fileBuffer);
            fileBuffer = NULL;
        }

        ret = FSFILE_Close(fileHandle);

        printf("loaded code : %08X\n", (unsigned int)fileSize);
    }

    paramblk->code_data = NULL;
    paramblk->code_size = 0;

    if(fileBuffer)
    {
        u32 decompressedSize = lzss_get_decompressed_size(fileBuffer, fileSize);
        printf("decompressed code size : %08X\n", (unsigned int)decompressedSize);
        u8* decompressedBuffer = linearMemAlign(decompressedSize, 0x1000);
        if(!decompressedBuffer)return -1;

        lzss_decompress(fileBuffer, fileSize, decompressedBuffer, decompressedSize);

        buildMap(&processMap, decompressedSize);
        printf("map built : %08X\n", (unsigned int)(FIRM_APPMEMALLOC_LINEAR - processMap.processLinearOffset));

        paramblk->code_data = (u32*)decompressedBuffer;
        paramblk->code_size = decompressedSize;
    }
    
    static char code_path[128];
    static char url_buf[128];
    u32 tid_short = (unsigned int)(tid & 0xffffffff);
    snprintf(code_path, 128, "sdmc:/hans/%08X.code.orig", tid_short);
    
    FILE* file = fopen(code_path, "wb");
    fwrite(paramblk->code_data, sizeof(u8), paramblk->code_size, file);
    fclose(file);
    printf(".code written to %s\n", code_path);
    
    if(tid_short == 0x00055E00 || tid_short == 0x00055D00 || tid_short == 0x0011C400 || tid_short == 0x0011C500) // || tid_short == 0x00164800 || tid_short == 0x00175E00)
    {
        u32 ser_addr = 0;
        u32 boss1_addr = 0;
        u32 boss2_addr = 0;
        if(tid_short == 0x00055E00 || 0x00055D00) { // Y || X
            ser_addr = search_string("https://3ds1-fushigi.pokemon-gl.com/api/", 0x490000, 0x100000, paramblk->code_data);
            boss1_addr = search_string("https://npdl.cdn.nintendowifi.net/p01/nsa/%s/%s/%s", 0x490000, 0x100000, paramblk->code_data);
            boss2_addr = search_string("https://npfl.c.app.nintendowifi.net/p01/filelist/", 0x490000, 0x100000, paramblk->code_data);
        }
        else if(tid_short == 0x0011C400 || 0x0011C500) { // OR || AS
            ser_addr = search_string("https://3ds2-fushigi.pokemon-gl.com/api/", 0x4E0000, 0x100000, paramblk->code_data);
            boss1_addr = search_string("https://npdl.cdn.nintendowifi.net/p01/nsa/%s/%s/%s", 0x4E0000, 0x100000, paramblk->code_data);
            boss2_addr = search_string("https://npfl.c.app.nintendowifi.net/p01/filelist/", 0x4E0000, 0x100000, paramblk->code_data);
        }
        // else { // (tid_short == 0x00164800 || 0x00175E00) aka SUN || MOON
        //     u32 ser_addr = search_string("https://3ds3-fushigi.pokemon-gl.com/api/", , 0x100000, paramblk->code_data);
        // }

        printf("Searched and found: %x %x %x\n", ser_addr, boss1_addr, boss2_addr);
        
        static char *base_url = "mys.salthax.org";
        
        memset((u8*)(paramblk->code_data)+ser_addr*sizeof(u8), 0, strlen("https://3ds*-fushigi.pokemon-gl.com/api/"));
        sprintf((u8*)(paramblk->code_data)+ser_addr*sizeof(u8), "http://%s/api/", base_url);
        //printf("%s\n", (u8*)(paramblk->code_data)+ser_addr*sizeof(u8));
        
        memset((u8*)(paramblk->code_data)+boss1_addr*sizeof(u8), 0, strlen("https://npdl.cdn.nintendowifi.net/p01/nsa/%s/%s/%s"));
        sprintf((u8*)(paramblk->code_data)+boss1_addr*sizeof(u8), "http://%s/p01/nsa/%%s/%%s/%%s", base_url);
        //printf("%s\n", (u8*)(paramblk->code_data)+boss1_addr*sizeof(u8));
        
        memset((u8*)(paramblk->code_data)+boss2_addr*sizeof(u8), 0, strlen("https://npfl.c.app.nintendowifi.net/p01/filelist/"));
        sprintf((u8*)(paramblk->code_data)+boss2_addr*sizeof(u8), "http://%s/p01/filelist/", base_url);
        //printf("%s\n", (u8*)(paramblk->code_data)+boss2_addr*sizeof(u8));
        
        snprintf(code_path, 128, "sdmc:/hans/%08X.code", tid_short);
        
        FILE* file_2 = fopen(code_path, "wb");
        fwrite(paramblk->code_data, sizeof(u8), paramblk->code_size, file_2);
        fclose(file_2);
        printf("modified .code written to %s\n", code_path);
    }

    memorymap_t* _mmap = loadMemoryMapTitle(tid & 0xffffffff, (tid >> 32) & 0xffffffff);
    if(_mmap)
    {
        processMap = *_mmap;
        free(_mmap);
    }

    return 0;
}

Result NSS_LaunchTitle(Handle* handle, u64 tid, u8 flags)
{
    if(!handle)return -1;

    u32* cmdbuf=getThreadCommandBuffer();
    cmdbuf[0]=0x000200C0; //request header code
    cmdbuf[1]=tid&0xFFFFFFFF;
    cmdbuf[2]=(tid>>32)&0xFFFFFFFF;
    cmdbuf[3]=flags;

    Result ret=0;
    if((ret=svcSendSyncRequest(*handle)))return ret;

    return cmdbuf[1];
}

Result NSS_TerminateProcessTID(Handle* handle, u64 tid, u64 timeout)
{
    if(!handle)return -1;

    u32* cmdbuf=getThreadCommandBuffer();
    cmdbuf[0]=0x00110100; //request header code
    cmdbuf[1]=tid&0xFFFFFFFF;
    cmdbuf[2]=(tid>>32)&0xFFFFFFFF;
    cmdbuf[3]=timeout&0xFFFFFFFF;
    cmdbuf[4]=(timeout>>32)&0xFFFFFFFF;

    Result ret=0;
    if((ret=svcSendSyncRequest(*handle)))return ret;

    return cmdbuf[1];
}

extern void (*__system_retAddr)(void);

extern u32 __linear_heap;
extern u32 __heap_size, __linear_heap_size;
extern void (*__system_retAddr)(void);

void __appExit();

void __libc_fini_array(void);

void __appInit() {
    // Initialize services
    srvInit();
    // aptInit();
    hidInit();

    fsInit();
    sdmcInit();
}

void __appExit() {
    // Exit services
    sdmcExit();
    fsExit();

    hidExit();
    // aptExit();
    srvExit();
}

PrintConsole topScreen, bottomScreen;

int main(int argc, char **argv)
{
    gfxInitDefault();

    consoleInit(GFX_TOP, &topScreen);
    consoleInit(GFX_BOTTOM, &bottomScreen);

    consoleSelect(&topScreen);

    printf("what is up\n");

    paramblk = linearAlloc(sizeof(paramblk_t));
    srvGetServiceHandle(&paramblk->nssHandle, "ns:s");
    loadCode();
    printf("code dumped, press start to exit");
    
    while(true)
    {
        hidScanInput();
        if(hidKeysDown() & KEY_START)break;
    }

    gfxExit();
    return 0;
}

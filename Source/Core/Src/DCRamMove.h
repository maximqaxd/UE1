// KOS ramdisk has no rename callback. Transfer ownership without copying the
// snapshot; its detach/attach APIs take paths relative to the /ram mount.
static UBOOL DCMoveRamFile(const char* Src,const char* Dest)
{
    if(!appStrcmp(Src,Dest))return appFSize(Src)>=0;
    void* Source=NULL;size_t SourceSize=0;
    if(fs_ramdisk_detach(Src+5,&Source,&SourceSize)<0)return 0;
    void* Previous=NULL;size_t PreviousSize=0;
    const UBOOL HadPrevious=appFSize(Dest)>=0;
    if(HadPrevious && fs_ramdisk_detach(Dest+5,&Previous,&PreviousSize)<0)
    {
        if(fs_ramdisk_attach(Src+5,Source,SourceSize)<0)free(Source);
        return 0;
    }
    if(fs_ramdisk_attach(Dest+5,Source,SourceSize)==0)
    {
        free(Previous);
        return 1;
    }
    // Failed staging must not discard an older destination snapshot.
    if(HadPrevious && fs_ramdisk_attach(Dest+5,Previous,PreviousSize)<0)
    {
        debugf(NAME_Warning,"Cannot restore RAM destination '%s'",Dest);
        free(Previous);
    }
    if(fs_ramdisk_attach(Src+5,Source,SourceSize)<0)
    {
        debugf(NAME_Warning,"Cannot restore RAM source '%s'",Src);
        free(Source);
    }
    return 0;
}

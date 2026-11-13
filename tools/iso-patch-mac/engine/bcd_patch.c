/*
 * bcd_patch.c -- build a Windows-boot BCD from BCD-Template. See bcd_patch.h and
 * docs/appsandbox/BCD-BUILD-SPEC.md (regf + device-element layout).
 *
 * Strategy: copy the template, then (a) point {bootmgr}.default at the template's
 * own OS-loader object (found by Type 0x10200003 — never hardcoded), (b) CREATE
 * the device/osdevice/displayorder/path elements the template lacks by carving
 * cells into a freshly appended hbin, (c) fix sequence numbers + XOR checksum.
 * Every raw pointer is recomputed after any cell_alloc (realloc may move the buf).
 */
#include "bcd_patch.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

typedef struct { uint8_t *b; size_t len, cap; uint32_t abump, aend; } hive_t;

static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }
static uint32_t rd32(const uint8_t *p){ return p[0]|(p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void wr16(uint8_t *p, uint16_t v){ p[0]=v; p[1]=v>>8; }
static void wr32(uint8_t *p, uint32_t v){ p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
static uint32_t ru8(uint32_t x){ return (x+7)&~7u; }

#define NIL 0xFFFFFFFFu
#define BODY(h,off) ((h)->b + 0x1000 + (off) + 4)   /* record body of cell at stored off */

static int hive_name_compare(const char *a, const char *b){
    for (;; a++, b++) {
        unsigned ca=(unsigned char)*a, cb=(unsigned char)*b;
        if (ca>='a' && ca<='z') ca-='a'-'A';
        if (cb>='a' && cb<='z') cb-='a'-'A';
        if (ca!=cb) return ca<cb?-1:1;
        if (!ca) return 0;
    }
}

/* ---- read-side navigation ---- */
static void nk_name(hive_t *h, uint32_t nk, char *out, int cap){
    uint8_t *b = BODY(h,nk); uint16_t fl=rd16(b+0x02), nl=rd16(b+0x48); uint8_t *nm=b+0x4C; int j=0;
    if (fl&0x20){ for (int i=0;i<nl && j<cap-1;i++){ char c=nm[i]; out[j++]=c; } }
    else        { for (int i=0;i+1<nl && j<cap-1;i+=2){ char c=nm[i]; out[j++]=c; } }
    out[j]=0;
}
static int collect(hive_t *h, uint32_t list, uint32_t *out, int cap){
    if (list==NIL) return 0;
    uint8_t *b=BODY(h,list); uint16_t cnt=rd16(b+0x02);
    if (!memcmp(b,"ri",2)){ int n=0; for (int i=0;i<cnt;i++){ n+=collect(h, rd32(b+4+4*i), out+n, cap-n); } return n; }
    int esz = (!memcmp(b,"li",2))?4:8, n=0;
    for (int i=0;i<cnt && n<cap;i++) out[n++]=rd32(b+4+esz*i);
    return n;
}
static int children(hive_t *h, uint32_t nk, uint32_t *out, int cap){
    return collect(h, rd32(BODY(h,nk)+0x1C), out, cap);
}
static uint32_t find_subkey(hive_t *h, uint32_t nk, const char *name){
    uint32_t kids[4096]; int n=children(h,nk,kids,4096);
    for (int i=0;i<n;i++){ char nm[128]; nk_name(h,kids[i],nm,sizeof nm); if (!hive_name_compare(nm,name)) return kids[i]; }
    return NIL;
}
static uint32_t find_value(hive_t *h, uint32_t nk, const char *name){
    uint8_t *b=BODY(h,nk); uint32_t nval=rd32(b+0x24); if (!nval) return NIL;
    uint32_t vlist=rd32(b+0x28); uint8_t *ve=BODY(h,vlist);
    for (uint32_t i=0;i<nval;i++){ uint32_t vk=rd32(ve+4*i); uint8_t *vb=BODY(h,vk);
        uint16_t nl=rd16(vb+0x02), vf=rd16(vb+0x10); char nm[64]; int j=0;
        if (vf&1){ for (int k=0;k<nl&&j<63;k++) nm[j++]=vb[0x14+k]; } else { for (int k=0;k+1<nl&&j<63;k+=2) nm[j++]=vb[0x14+k]; }
        nm[j]=0; if (!hive_name_compare(nm,name)) return vk; }
    return NIL;
}
static const uint8_t *vk_data(hive_t *h, uint32_t vk, uint32_t *len){
    uint8_t *vb=BODY(h,vk); uint32_t ds=rd32(vb+0x04); *len=ds&0x7FFFFFFF;
    if (ds&0x80000000) return vb+0x08; return BODY(h, rd32(vb+0x08));
}

/* ---- write-side: append hbin + carve cells ---- */
static void hbin_append(hive_t *h, uint32_t need){
    uint32_t hbins = rd32(h->b+0x28);
    uint32_t size = (need + 0x20 + 0xFFF) & ~0xFFFu;
    size_t nl = h->len + size;
    if (nl > h->cap){ h->cap = nl*2; h->b = realloc(h->b, h->cap); }
    uint8_t *hb = h->b + h->len; memset(hb,0,size);
    memcpy(hb,"hbin",4); wr32(hb+4, hbins); wr32(hb+8, size);
    wr32(h->b+0x28, hbins + size);
    h->abump = (uint32_t)(h->len + 0x20);
    h->aend  = (uint32_t)(h->len + size);
    h->len = nl;
}
/* returns stored offset of a new allocated cell holding `payload` bytes (zeroed) */
static uint32_t cell_alloc(hive_t *h, uint32_t payload){
    uint32_t need = ru8(4+payload);
    if (h->aend==0 || h->abump+need > h->aend){
        if (h->aend && h->abump < h->aend) wr32(h->b+h->abump, h->aend - h->abump); /* free tail */
        hbin_append(h, need);
    }
    uint32_t fo = h->abump;
    wr32(h->b+fo, (uint32_t)(-(int32_t)need));
    memset(h->b+fo+4, 0, need-4);
    h->abump += need;
    return fo - 0x1000;
}

static void sublist_add(hive_t *h, uint32_t parent, uint32_t child, const char *name){
    (void)name;
    uint32_t kids[4096]; int n = collect(h, rd32(BODY(h,parent)+0x1C), kids, 4095);
    kids[n++] = child;
    for (int i=1;i<n;i++){ uint32_t k=kids[i]; char ki[128]; nk_name(h,k,ki,sizeof ki); int j=i-1;
        for (; j>=0; j--){ char kj[128]; nk_name(h,kids[j],kj,sizeof kj); if (hive_name_compare(kj,ki)>0) kids[j+1]=kids[j]; else break; }
        kids[j+1]=k; }
    uint32_t lf = cell_alloc(h, 4 + 8*n);
    uint8_t *lb = BODY(h,lf); memcpy(lb,"lf",2); wr16(lb+2,(uint16_t)n);
    for (int i=0;i<n;i++){ wr32(lb+4+8*i, kids[i]); char nm[8]={0}; nk_name(h,kids[i],nm,5); memcpy(lb+4+8*i+4, nm, 4); }
    uint8_t *pb = BODY(h,parent); wr32(pb+0x1C, lf); wr32(pb+0x14, (uint32_t)n);
}

/* Remove every TEMPLATE-class (0x4xxxxxxx) element from an Elements subkey. A live
 * BCD must carry NONE (bcdboot emits none): the template ships them only as the
 * SOURCE values bcdboot transforms into live library/application-class elements.
 * Leaving them makes bootmgr reject the object's element list -> 0xc0000001 (it
 * can't enumerate the OS entry, so no boot menu appears). Rebuilds the 'lf' subkey
 * list keeping only non-'4'-prefixed children (removal preserves sort order). */
static void strip_template_elems(hive_t *h, uint32_t elements){
    uint32_t kids[4096]; int n=children(h,elements,kids,4096);
    uint32_t keep[4096]; int m=0;
    for (int i=0;i<n;i++){ char nm[16]; nk_name(h,kids[i],nm,sizeof nm); if (nm[0]!='4') keep[m++]=kids[i]; }
    if (m==n) return;
    uint32_t lf = cell_alloc(h, 4 + 8*m);
    uint8_t *lb = BODY(h,lf); memcpy(lb,"lf",2); wr16(lb+2,(uint16_t)m);
    for (int i=0;i<m;i++){ wr32(lb+4+8*i, keep[i]); char nm[8]={0}; nk_name(h,keep[i],nm,5); memcpy(lb+4+8*i+4, nm, 4); }
    uint8_t *pb = BODY(h,elements); wr32(pb+0x1C, lf); wr32(pb+0x14, (uint32_t)m);
}

/* Remove an object key `target` from the Objects subkey list (rebuild its 'lf'
 * without it). Used to drop the template's orphan WinPE/Setup ramdisk loader
 * ({...} Type 0x10200003 with 42000003) that a clean bcdboot store never carries.
 * The removed object's cells are simply left unreferenced (a few hundred bytes;
 * the hive stays consistent -- cells still tile, just nothing points at them). */
static void remove_object(hive_t *h, uint32_t objects, uint32_t target){
    uint32_t kids[4096]; int n=children(h,objects,kids,4096);
    uint32_t keep[4096]; int m=0;
    for (int i=0;i<n;i++) if (kids[i]!=target) keep[m++]=kids[i];
    if (m==n) return;                                /* not found */
    uint32_t lf = cell_alloc(h, 4 + 8*m);
    uint8_t *lb = BODY(h,lf); memcpy(lb,"lf",2); wr16(lb+2,(uint16_t)m);
    for (int i=0;i<m;i++){ wr32(lb+4+8*i, keep[i]); char nm[8]={0}; nk_name(h,keep[i],nm,5); memcpy(lb+4+8*i+4, nm, 4); }
    uint8_t *pb = BODY(h,objects); wr32(pb+0x1C, lf); wr32(pb+0x14, (uint32_t)m);
}

/* Create-or-update Elements\<name>\Element. type: 1=SZ,3=BINARY,7=MULTI_SZ. */
static void set_element(hive_t *h, uint32_t elements, const char *name, uint32_t type,
                        const uint8_t *data, uint32_t dlen){
    uint32_t esub = find_subkey(h, elements, name);
    uint32_t dcell = cell_alloc(h, dlen); memcpy(BODY(h,dcell), data, dlen);
    if (esub != NIL){                              /* update existing Element vk */
        uint32_t vk = find_value(h, esub, "Element");
        uint8_t *vb = BODY(h,vk); wr32(vb+0x04, dlen); wr32(vb+0x08, dcell); wr32(vb+0x0C, type);
        return;
    }
    uint32_t vk = cell_alloc(h, 0x14+7);
    { uint8_t *vb=BODY(h,vk); memcpy(vb,"vk",2); wr16(vb+0x02,7); wr32(vb+0x04,dlen); wr32(vb+0x08,dcell);
      wr32(vb+0x0C,type); wr16(vb+0x10,1); memcpy(vb+0x14,"Element",7); }
    uint32_t vlist = cell_alloc(h, 4); wr32(BODY(h,vlist), vk);
    uint32_t sk; { uint8_t *eb=BODY(h,elements); sk=rd32(eb+0x2C); }
    int nl=(int)strlen(name); uint32_t nk = cell_alloc(h, 0x4C+nl);
    { uint8_t *nb=BODY(h,nk); memset(nb,0,0x4C); memcpy(nb,"nk",2); wr16(nb+0x02,0x20);
      wr32(nb+0x10,elements); wr32(nb+0x1C,NIL); wr32(nb+0x24,1); wr32(nb+0x28,vlist);
      wr32(nb+0x2C,sk); wr32(nb+0x30,NIL); wr16(nb+0x48,(uint16_t)nl); memcpy(nb+0x4C,name,nl); }
    sublist_add(h, elements, nk, name);
}

/* Create a brand-new BCD object (a GUID key under Objects) with a Description\Type
 * value and an empty Elements subkey; returns the Elements cell offset so the
 * caller can set_element() into it. Used to add {fwbootmgr}, which the WIM's
 * BCD-Template omits but a UEFI SYSTEM store must have (else sysprep's
 * Sysprep_Online_Specialize_Bcd rejects the store -> "not system store"). All raw
 * pointers are recomputed after each cell_alloc (realloc may move the buffer). */
static uint32_t create_object(hive_t *h, uint32_t objects, const char *guid, uint32_t type){
    uint32_t sk; { sk = rd32(BODY(h,objects)+0x2C); }              /* reuse Objects' security cell */
    int gl=(int)strlen(guid);
    uint32_t obj = cell_alloc(h, 0x4C+gl);                          /* the {guid} object key */
    /* Description\Type = REG_DWORD(type) */
    uint32_t dcell = cell_alloc(h, 4); wr32(BODY(h,dcell), type);
    uint32_t tvk = cell_alloc(h, 0x14+4);
    { uint8_t *vb=BODY(h,tvk); memcpy(vb,"vk",2); wr16(vb+0x02,4); wr32(vb+0x04,4); wr32(vb+0x08,dcell);
      wr32(vb+0x0C,4 /*REG_DWORD*/); wr16(vb+0x10,1); memcpy(vb+0x14,"Type",4); }
    uint32_t tvlist = cell_alloc(h,4); wr32(BODY(h,tvlist), tvk);
    uint32_t desc = cell_alloc(h, 0x4C+11);
    { uint8_t *nb=BODY(h,desc); memset(nb,0,0x4C); memcpy(nb,"nk",2); wr16(nb+0x02,0x20);
      wr32(nb+0x10,obj); wr32(nb+0x1C,NIL); wr32(nb+0x24,1); wr32(nb+0x28,tvlist);
      wr32(nb+0x2C,sk); wr32(nb+0x30,NIL); wr16(nb+0x48,11); memcpy(nb+0x4C,"Description",11); }
    uint32_t elem = cell_alloc(h, 0x4C+8);                          /* empty Elements */
    { uint8_t *nb=BODY(h,elem); memset(nb,0,0x4C); memcpy(nb,"nk",2); wr16(nb+0x02,0x20);
      wr32(nb+0x10,obj); wr32(nb+0x14,0); wr32(nb+0x1C,NIL); wr32(nb+0x24,0); wr32(nb+0x28,NIL);
      wr32(nb+0x2C,sk); wr32(nb+0x30,NIL); wr16(nb+0x48,8); memcpy(nb+0x4C,"Elements",8); }
    /* obj's children = [Description, Elements] (sorted) */
    uint32_t lf = cell_alloc(h, 4+8*2);
    { uint8_t *lb=BODY(h,lf); memcpy(lb,"lf",2); wr16(lb+2,2);
      wr32(lb+4, desc); memcpy(lb+8,"Desc",4);
      wr32(lb+12, elem); memcpy(lb+16,"Elem",4); }
    { uint8_t *nb=BODY(h,obj); memset(nb,0,0x4C); memcpy(nb,"nk",2); wr16(nb+0x02,0x20);
      wr32(nb+0x10,objects); wr32(nb+0x14,2); wr32(nb+0x1C,lf); wr32(nb+0x24,0); wr32(nb+0x28,NIL);
      wr32(nb+0x2C,sk); wr32(nb+0x30,NIL); wr16(nb+0x48,(uint16_t)gl); memcpy(nb+0x4C,guid,gl); }
    sublist_add(h, objects, obj, guid);                             /* link into Objects */
    return elem;
}

/* Add or update a REG_DWORD value `name`=`val` on key `nk` (inline data). Used to
 * stamp the root\Description with System=1 (+ FirmwareModified=1), which marks a
 * BCD as the SYSTEM store -- bcdboot/BCD-SYS write this; the WIM's BCD-Template
 * lacks it, so sysprep's Sysprep_Online_Specialize_Bcd rejects ours as "not system
 * store" (0xC0000098) -> "could not configure hardware". Pointers recomputed after
 * every cell_alloc (realloc may move the buffer). */
static void set_value(hive_t *h, uint32_t nk, const char *name, uint32_t type,
                      const uint8_t *data, uint32_t len){
    uint32_t vk = find_value(h, nk, name);
    uint32_t dc = 0;
    if (len > 4){ dc = cell_alloc(h, len); memcpy(BODY(h,dc), data, len); }
    if (vk == NIL){
        size_t nl = strlen(name);
        vk = cell_alloc(h, (uint32_t)(0x14 + nl));
        uint32_t count = rd32(BODY(h,nk)+0x24);
        uint32_t oldlist = rd32(BODY(h,nk)+0x28);
        uint32_t list = cell_alloc(h, 4*(count+1));
        if (count) memcpy(BODY(h,list), BODY(h,oldlist), 4*count);
        wr32(BODY(h,list)+4*count, vk);
        wr32(BODY(h,nk)+0x24, count+1); wr32(BODY(h,nk)+0x28, list);
        uint8_t *v = BODY(h,vk);
        memcpy(v,"vk",2); wr16(v+2,(uint16_t)nl); wr16(v+0x10,1);
        memcpy(v+0x14,name,nl);
    }
    uint8_t *v = BODY(h,vk);
    wr32(v+4, len | (len <= 4 ? 0x80000000u : 0));
    wr32(v+8, dc); wr32(v+0x0C, type);
    if (len <= 4 && len) memcpy(v+8,data,len);
    uint32_t name_len = (uint32_t)strlen(name)*2;
    if (rd32(BODY(h,nk)+0x3C) < name_len) wr32(BODY(h,nk)+0x3C,name_len);
    if (rd32(BODY(h,nk)+0x40) < len) wr32(BODY(h,nk)+0x40,len);
}

static void set_dword_value(hive_t *h, uint32_t nk, const char *name, uint32_t val){
    uint8_t data[4]; wr32(data,val); set_value(h,nk,name,4,data,4);
}

/* ---- BCD-specific helpers ---- */
static void device_blob(uint8_t out[88], const uint8_t part[16], const uint8_t disk[16]){
    memset(out,0,88); out[0x10]=0x06; out[0x18]=0x48; memcpy(out+0x20,part,16); memcpy(out+0x38,disk,16);
}
static uint32_t str_utf16(const char *s, uint8_t *out, int multisz){
    int n=(int)strlen(s);
    for (int i=0;i<n;i++){ out[2*i]=(uint8_t)s[i]; out[2*i+1]=0; }
    out[2*n]=0; out[2*n+1]=0;
    uint32_t len = 2*(n+1);
    if (multisz){ out[len]=0; out[len+1]=0; len+=2; }
    return len;
}
/* Find the first object with Description\Type==type that HAS need_elem (if non-NULL)
 * and LACKS excl_elem (if non-NULL). Used to single out the installed-OS loader
 * (Type 0x10200003, has 22000002 systemroot, lacks 42000003 ramdisk-wim path) from
 * the WinPE/Setup ramdisk loader, ISO-agnostically. */
static uint32_t find_object_by_type(hive_t *h, uint32_t objects, uint32_t type,
                                    const char *need_elem, const char *excl_elem){
    uint32_t kids[256]; int n=children(h,objects,kids,256);
    for (int i=0;i<n;i++){
        uint32_t desc=find_subkey(h,kids[i],"Description"); if (desc==NIL) continue;
        uint32_t tvk=find_value(h,desc,"Type"); if (tvk==NIL) continue;
        uint32_t l; const uint8_t *d=vk_data(h,tvk,&l); if (l<4 || rd32(d)!=type) continue;
        uint32_t el=find_subkey(h,kids[i],"Elements");
        if (need_elem){ if (el==NIL || find_subkey(h,el,need_elem)==NIL) continue; }
        if (excl_elem){ if (el!=NIL && find_subkey(h,el,excl_elem)!=NIL) continue; }
        return kids[i];
    }
    return NIL;
}

int bcd_build(const uint8_t *tpl, size_t tpl_len, const bcd_guids_t *g, uint8_t **out, size_t *out_len){
    if (tpl_len < 0x1000 || memcmp(tpl,"regf",4)) return -1;
    hive_t hv; hv.cap = tpl_len + 0x4000; hv.b = malloc(hv.cap); if(!hv.b) return -1; memcpy(hv.b,tpl,tpl_len);
    hv.len = tpl_len; hv.abump = 0; hv.aend = 0;
    hive_t *h=&hv;

    uint32_t root = rd32(h->b+0x24);
    uint32_t objects = find_subkey(h, root, "Objects");
    if (objects==NIL){ free(h->b); return -2; }

    /* {bootmgr} (Type 0x10100002) + the INSTALLED-OS loader (Type 0x10200003 with
     * a \windows systemroot but NOT the \boot.wim ramdisk path of the WinPE loader). */
    uint32_t bootmgr = find_object_by_type(h, objects, 0x10100002, NULL, NULL);
    uint32_t loader  = find_object_by_type(h, objects, 0x10200003, "22000002", "42000003");
    if (loader==NIL) loader = find_object_by_type(h, objects, 0x10200003, "22000002", NULL);
    if (loader==NIL) loader = find_object_by_type(h, objects, 0x10200003, NULL, NULL);
    if (bootmgr==NIL || loader==NIL){ free(h->b); return -3; }

    char loader_guid[64]; nk_name(h, loader, loader_guid, sizeof loader_guid);
    uint32_t bm_elems = find_subkey(h, bootmgr, "Elements");
    uint32_t ld_elems = find_subkey(h, loader,  "Elements");
    if (bm_elems==NIL || ld_elems==NIL){ free(h->b); return -4; }

    uint8_t blob[88], buf[256], qw[8]; uint32_t l;

    /* Strip the template-class (0x4xxxxxxx) placeholders the template ships in both
     * objects; bcdboot's live BCD has none. Do this BEFORE adding live elements. */
    strip_template_elems(h, bm_elems);
    strip_template_elems(h, ld_elems);

    /* {bootmgr}: device -> ESP, default(23000003)=loader, displayorder(24000001)=[loader].
     * Keeps the template's 12000004(desc)/12000005(locale)/14000006(inherit->global). */
    device_blob(blob, g->esp_part_guid, g->disk_guid);
    set_element(h, bm_elems, "11000001", 3, blob, 88);
    l = str_utf16(loader_guid, buf, 0); set_element(h, bm_elems, "23000003", 1, buf, l);
    l = str_utf16(loader_guid, buf, 1); set_element(h, bm_elems, "24000001", 7, buf, l);
    l = str_utf16("\\EFI\\Microsoft\\Boot\\bootmgfw.efi", buf, 0);
    set_element(h, bm_elems, "12000002", 1, buf, l);
    /* timeout: 30s as an 8-byte LE qword (template ships a 1-byte value; bcdboot
     * normalizes every INTEGER (format 0x5) element to 8 bytes). */
    memset(qw,0,8); qw[0]=30; set_element(h, bm_elems, "25000004", 3, qw, 8);

    /* loader: device + osdevice -> Windows, winload path, description, inherit. */
    device_blob(blob, g->win_part_guid, g->disk_guid);
    set_element(h, ld_elems, "11000001", 3, blob, 88);
    set_element(h, ld_elems, "21000001", 3, blob, 88);
    l = str_utf16("\\Windows\\System32\\winload.efi", buf, 0);
    set_element(h, ld_elems, "12000002", 1, buf, l);
    /* description: bootmgr needs 12000004 to render the boot-menu entry. */
    l = str_utf16("Windows 11", buf, 0); set_element(h, ld_elems, "12000004", 1, buf, l);
    /* inherit {bootloadersettings} -> global + hypervisor (bcdboot always writes this). */
    l = str_utf16("{6efb52bf-1766-41db-a6b3-0ee5eff72bd7}", buf, 1);
    set_element(h, ld_elems, "14000006", 7, buf, l);
    /* nx=OptIn(0) and bootmenupolicy=Standard(1) as 8-byte qwords (template ships
     * these 1-byte; bcdboot normalizes INTEGER elements to 8 bytes). */
    memset(qw,0,8);            set_element(h, ld_elems, "25000020", 3, qw, 8);
    memset(qw,0,8); qw[0]=1;   set_element(h, ld_elems, "250000c2", 3, qw, 8);

    /* Optional serial kernel debugging, baked in at BUILD time (never edit the
     * FAT ESP post-hoc — that corrupts it -> FatDiskIo timeouts -> 0xc000000d).
     * This is `bcdedit /debug on` + serial dbgsettings ONLY: NO bootdebug (which
     * halts winload waiting for a debugger) and NO EMS. Windows boots NORMALLY
     * to OOBE/desktop while the kernel emits KD/bugcheck output over the SPCR
     * serial (PL011 on ARM64 -> the QEMU -serial sink). Enable: WIN_DISK_DEBUG_SERIAL=1. */
    if (getenv("WIN_DISK_DEBUG_SERIAL")){
        uint8_t one=1;
        set_element(h, ld_elems, "16000010", 3, &one, 1);                  /* DebuggerEnabled = true   */
        memset(qw,0,8);                    set_element(h, ld_elems, "15000011", 3, qw, 8); /* type=SERIAL */
        memset(qw,0,8); qw[0]=1;           set_element(h, ld_elems, "15000012", 3, qw, 8); /* SerialPort=1 */
        memset(qw,0,8); qw[0]=0x00; qw[1]=0xC2; qw[2]=0x01;
                                           set_element(h, ld_elems, "15000013", 3, qw, 8); /* Baud=115200 */
    }

    /* ---- clean up to a true bcdboot-equivalent store ----
     * (a) Convert the template's resume object (Type 0x10200004) into a live
     *     resume loader: drop its template-class (4xxxxxxx) source elements and
     *     give it real device/osdevice (the Windows volume) + winresume path. It
     *     keeps the template's 12000004/12000005/14000006/16000060/17000077/
     *     22000002(\hiberfil.sys)/25000008. (b) Point {bootmgr}\23000006 at it.
     *     (c) Remove the template's orphan WinPE/Setup loader (has 42000003 ->
     *     \boot.wim) that a real bcdboot store never carries.
     * Win11 ARM64 ships no memtest.efi, so (correctly) NO {memdiag} object. */
    uint32_t resume = find_object_by_type(h, objects, 0x10200004, NULL, NULL);
    if (resume != NIL){
        char resume_guid[64]; nk_name(h, resume, resume_guid, sizeof resume_guid);  /* "{...}" */
        uint32_t rs_elems = find_subkey(h, resume, "Elements");
        if (rs_elems != NIL){
            strip_template_elems(h, rs_elems);
            device_blob(blob, g->win_part_guid, g->disk_guid);
            set_element(h, rs_elems, "11000001", 3, blob, 88);  /* device   -> Windows volume */
            set_element(h, rs_elems, "21000001", 3, blob, 88);  /* osdevice -> Windows volume */
            l = str_utf16("\\Windows\\system32\\winresume.efi", buf, 0);
            set_element(h, rs_elems, "12000002", 1, buf, l);    /* path */
        }
        char braced[80]; snprintf(braced, sizeof braced, "%s", resume_guid);
        l = str_utf16(braced, buf, 0);
        set_element(h, bm_elems, "23000006", 1, buf, l);        /* {bootmgr} resume object ref */
    }
    /* drop the orphan WinPE/Setup ramdisk loader (Type 0x10200003 WITH 42000003) */
    {
        uint32_t winpe = find_object_by_type(h, objects, 0x10200003, "42000003", NULL);
        if (winpe != NIL && winpe != loader) remove_object(h, objects, winpe);
    }

    /* {fwbootmgr} (Type 0x10100001): the UEFI firmware boot manager object. The
     * BCD-Template omits it, but a real UEFI SYSTEM store (bcdboot output) has it,
     * and sysprep's Sysprep_Online_Specialize_Bcd requires it to accept the store
     * as the system store -- without it: "File is not system store" (0xC0000098)
     * -> "Windows Setup could not configure Windows to run on this hardware".
     * Give it displayorder -> {bootmgr} and a 30s timeout, like bcdboot. */
    if (find_object_by_type(h, objects, 0x10100001, NULL, NULL) == NIL){
        char bm_guid[64]; nk_name(h, bootmgr, bm_guid, sizeof bm_guid);
        uint32_t fw_elems = create_object(h, objects, "{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}", 0x10100001);
        l = str_utf16(bm_guid, buf, 1); set_element(h, fw_elems, "24000001", 7, buf, l); /* displayorder */
        memset(qw,0,8); qw[0]=30; set_element(h, fw_elems, "25000004", 3, qw, 8);         /* timeout 30s */
    }

    /* Mark this store as the SYSTEM store: root\Description += System=dword:1
     * (and FirmwareModified=1). KeyName=BCD00000000 is already in the template.
     * This is the value sysprep's BCD-specialize reads to accept the store as the
     * system store (matches bcdboot / BCD-SYS); without it -> "not system store". */
    {
        uint32_t rootdesc = find_subkey(h, root, "Description");
        if (rootdesc != NIL){
            set_dword_value(h, rootdesc, "System", 1);
            set_dword_value(h, rootdesc, "FirmwareModified", 1);
        }
    }

    /* finalize: free tail of last appended hbin, equalize seq, recompute checksum */
    if (h->aend && h->abump < h->aend) wr32(h->b+h->abump, h->aend - h->abump);
    wr32(h->b+0x08, rd32(h->b+0x04));
    uint32_t s=0; for (int i=0;i<127;i++) s ^= rd32(h->b+4*i);
    if (s==0xFFFFFFFF) s=0xFFFFFFFE; if (s==0) s=1; wr32(h->b+0x1FC, s);

    *out = h->b; *out_len = h->len;
    return 0;
}

static uint32_t ensure_key(hive_t *h, uint32_t parent, const char *name){
    uint32_t key = find_subkey(h,parent,name);
    if (key != NIL) return key;
    if (rd32(BODY(h,parent)+0x14) >= 4095) return NIL;
    size_t len = strlen(name);
    uint32_t sk = rd32(BODY(h,parent)+0x2C);
    key = cell_alloc(h,(uint32_t)(0x4C+len));
    uint8_t *b = BODY(h,key);
    memcpy(b,"nk",2); wr16(b+2,0x20);
    memcpy(b+4,BODY(h,parent)+4,8);
    wr32(b+0x10,parent); wr32(b+0x1C,NIL); wr32(b+0x20,NIL);
    wr32(b+0x28,NIL); wr32(b+0x2C,sk); wr32(b+0x30,NIL);
    wr16(b+0x48,(uint16_t)len); memcpy(b+0x4C,name,len);
    if (sk != NIL) wr32(BODY(h,sk)+0x0C,rd32(BODY(h,sk)+0x0C)+1);
    sublist_add(h,parent,key,name);
    uint32_t longest = rd32(BODY(h,parent)+0x34);
    if (longest < len*2) wr32(BODY(h,parent)+0x34,(uint32_t)len*2);
    return key;
}

static uint32_t ensure_path(hive_t *h, uint32_t root, const char *path){
    char copy[512];
    if (strlen(path) >= sizeof copy) return NIL;
    strcpy(copy,path);
    char *save = NULL;
    for (char *part = strtok_r(copy,"\\",&save); part; part = strtok_r(NULL,"\\",&save)) {
        root = ensure_key(h,root,part);
        if (root == NIL) break;
    }
    return root;
}

static void set_string(hive_t *h, uint32_t key, const char *name, uint32_t type, const char *value){
    uint8_t data[512];
    uint32_t len = str_utf16(value,data,type == 7);
    set_value(h,key,name,type,data,len);
}

int windows_boot_vioscsi(const uint8_t *system, size_t system_len,
                         uint8_t **out, size_t *out_len){
    if (!system || !out || !out_len || system_len < 0x2000 ||
        system_len > 64*1024*1024 || memcmp(system,"regf",4) ||
        rd32(system+4) != rd32(system+8) || rd32(system+0x14) != 1 ||
        rd32(system+0x28) > system_len-0x1000) return -1;
    system_len = 0x1000 + (size_t)rd32(system+0x28);
    if (system_len < 0x2000) return -1;
    for (size_t off = 0x1000; off < system_len;) {
        const uint8_t *bin = system+off;
        if (system_len-off < 0x20 || memcmp(bin,"hbin",4) ||
            rd32(bin+4) != off-0x1000) return -1;
        uint32_t size = rd32(bin+8);
        if (size < 0x1000 || (size & 0xFFF) || size > system_len-off) return -1;
        for (uint32_t pos = 0x20; pos < size;) {
            int32_t cell = (int32_t)rd32(bin+pos);
            if (cell == INT32_MIN) return -1;
            uint32_t len = cell < 0 ? (uint32_t)-cell : (uint32_t)cell;
            if (len < 8 || (len & 7) || len > size-pos) return -1;
            pos += len;
        }
        off += size;
    }
    uint32_t root = rd32(system+0x24);
    if (root > system_len-0x1000-0x50 || memcmp(system+0x1004+root,"nk",2)) return -1;
    hive_t h = { .cap = system_len+8*1024*1024, .len = system_len };
    h.b = malloc(h.cap);
    if (!h.b) return -1;
    memcpy(h.b,system,system_len);
    uint32_t database = find_subkey(&h,root,"DriverDatabase");
    if (database == NIL) { free(h.b); return -1; }

    uint32_t controls[4096];
    int count = children(&h,root,controls,4096), prepared = 0;
    for (int i = 0; i < count; i++) {
        char name[128]; nk_name(&h,controls[i],name,sizeof name);
        if (strlen(name) != 13 || strncmp(name,"ControlSet",10) ||
            name[10] < '0' || name[10] > '9' || name[11] < '0' || name[11] > '9' ||
            name[12] < '0' || name[12] > '9') continue;
        if (++prepared > 32) { free(h.b); return -1; }
        uint32_t key = ensure_path(&h,controls[i],"Services\\vioscsi");
        if (key == NIL) { free(h.b); return -1; }
        set_dword_value(&h,key,"Type",1); set_dword_value(&h,key,"Start",0);
        set_dword_value(&h,key,"ErrorControl",1);
        set_string(&h,key,"Group",1,"SCSI miniport");
        set_string(&h,key,"ImagePath",2,"system32\\drivers\\vioscsi.sys");
        uint32_t params = ensure_path(&h,key,"Parameters\\PnpInterface");
        if (params == NIL) { free(h.b); return -1; }
        set_dword_value(&h,params,"5",1);
        params = ensure_key(&h,key,"Parameters");
        set_dword_value(&h,params,"BusType",10);
        set_dword_value(&h,params,"DmaRemappingCompatible",0);
    }
    if (!prepared) { free(h.b); return -1; }

    const char *package = ASB_VIOSCSI_DRIVER_PACKAGE;
    uint32_t key = ensure_path(&h,database,"DriverInfFiles\\vioscsi.inf");
    if (key == NIL) { free(h.b); return -1; }
    set_string(&h,key,"",7,package); set_string(&h,key,"Active",1,package);
    set_string(&h,key,"Configurations",7,"scsi_inst");
    char package_path[256]; snprintf(package_path,sizeof package_path,"DriverPackages\\%s",package);
    key = ensure_path(&h,database,package_path);
    if (key == NIL) { free(h.b); return -1; }
    const uint8_t version[48] = {0x00,0xFF,0x0C,0,0,0,0,0,
        0x7B,0xE9,0x36,0x4D,0x25,0xE3,0xCE,0x11,0xBF,0xC1,0x08,0,0x2B,0xE1,0x03,0x18};
    set_value(&h,key,"Version",3,version,sizeof version);
    uint32_t config = ensure_path(&h,key,"Configurations\\scsi_inst");
    if (config == NIL) { free(h.b); return -1; }
    set_dword_value(&h,config,"ConfigFlags",0);
    set_string(&h,config,"Service",1,"vioscsi");
    const char *ids[] = {"VEN_1AF4&DEV_1004", "VEN_1AF4&DEV_1004&REV_00",
                         "VEN_1AF4&DEV_1048", "VEN_1AF4&DEV_1048&REV_01"};
    for (size_t i = 0; i < sizeof ids/sizeof ids[0]; i++) {
        char path[256]; snprintf(path,sizeof path,"DeviceIds\\PCI\\%s",ids[i]);
        uint32_t device = ensure_path(&h,database,path);
        if (device == NIL) { free(h.b); return -1; }
        const uint8_t rank[] = {1,0xFF,0,0};
        set_value(&h,device,"vioscsi.inf",3,rank,sizeof rank);
        snprintf(path,sizeof path,"Descriptors\\PCI\\%s",ids[i]);
        device = ensure_path(&h,key,path);
        if (device == NIL) { free(h.b); return -1; }
        set_string(&h,device,"Configuration",1,"scsi_inst");
    }
    if (h.aend && h.abump < h.aend) wr32(h.b+h.abump,h.aend-h.abump);
    wr32(h.b+8,rd32(h.b+4));
    uint32_t checksum = 0;
    for (int i = 0; i < 127; i++) checksum ^= rd32(h.b+4*i);
    if (checksum == 0xFFFFFFFF) checksum = 0xFFFFFFFE;
    if (!checksum) checksum = 1;
    wr32(h.b+0x1FC,checksum);
    *out = h.b; *out_len = h.len;
    return 0;
}

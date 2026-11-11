#include "input_keys.h"
#include <dispatch/dispatch.h>
#include <pthread.h>

const uint16_t asb_mac_scan_codes[128] = {
    [kVK_ANSI_A]=0x1e, [kVK_ANSI_S]=0x1f, [kVK_ANSI_D]=0x20, [kVK_ANSI_F]=0x21,
    [kVK_ANSI_H]=0x23, [kVK_ANSI_G]=0x22, [kVK_ANSI_Z]=0x2c, [kVK_ANSI_X]=0x2d,
    [kVK_ANSI_C]=0x2e, [kVK_ANSI_V]=0x2f, [kVK_ANSI_B]=0x30, [kVK_ANSI_Q]=0x10,
    [kVK_ANSI_W]=0x11, [kVK_ANSI_E]=0x12, [kVK_ANSI_R]=0x13, [kVK_ANSI_Y]=0x15,
    [kVK_ANSI_T]=0x14, [kVK_ANSI_U]=0x16, [kVK_ANSI_I]=0x17, [kVK_ANSI_O]=0x18,
    [kVK_ANSI_P]=0x19, [kVK_ANSI_L]=0x26, [kVK_ANSI_J]=0x24, [kVK_ANSI_K]=0x25,
    [kVK_ANSI_N]=0x31, [kVK_ANSI_M]=0x32,
    [kVK_ANSI_1]=0x02, [kVK_ANSI_2]=0x03, [kVK_ANSI_3]=0x04, [kVK_ANSI_4]=0x05,
    [kVK_ANSI_5]=0x06, [kVK_ANSI_6]=0x07, [kVK_ANSI_7]=0x08, [kVK_ANSI_8]=0x09,
    [kVK_ANSI_9]=0x0a, [kVK_ANSI_0]=0x0b,
    [kVK_ANSI_Equal]=0x0d, [kVK_ANSI_Minus]=0x0c,
    [kVK_ANSI_LeftBracket]=0x1a, [kVK_ANSI_RightBracket]=0x1b,
    [kVK_ANSI_Quote]=0x28, [kVK_ANSI_Semicolon]=0x27, [kVK_ANSI_Backslash]=0x2b,
    [kVK_ANSI_Comma]=0x33, [kVK_ANSI_Slash]=0x35, [kVK_ANSI_Period]=0x34,
    [kVK_ANSI_Grave]=0x29, [kVK_ISO_Section]=0x56,
    [kVK_Return]=0x1c, [kVK_Tab]=0x0f, [kVK_Space]=0x39,
    [kVK_Delete]=0x0e, [kVK_Escape]=0x01,
    [kVK_Command]=0xe05b, [kVK_RightCommand]=0xe05c,
    [kVK_Shift]=0x2a, [kVK_RightShift]=0x36, [kVK_CapsLock]=0x3a,
    [kVK_Option]=0x38, [kVK_RightOption]=0xe038,
    [kVK_Control]=0x1d, [kVK_RightControl]=0xe01d,
    [kVK_ANSI_Keypad0]=0x52, [kVK_ANSI_Keypad1]=0x4f, [kVK_ANSI_Keypad2]=0x50,
    [kVK_ANSI_Keypad3]=0x51, [kVK_ANSI_Keypad4]=0x4b, [kVK_ANSI_Keypad5]=0x4c,
    [kVK_ANSI_Keypad6]=0x4d, [kVK_ANSI_Keypad7]=0x47, [kVK_ANSI_Keypad8]=0x48,
    [kVK_ANSI_Keypad9]=0x49, [kVK_ANSI_KeypadDecimal]=0x53,
    [kVK_ANSI_KeypadMultiply]=0x37, [kVK_ANSI_KeypadPlus]=0x4e,
    [kVK_ANSI_KeypadMinus]=0x4a, [kVK_ANSI_KeypadDivide]=0xe035,
    [kVK_ANSI_KeypadEnter]=0xe01c, [kVK_ANSI_KeypadClear]=0x45,
    [0x34]=0xe01c, [0x6e]=0xe05d,
    [kVK_F1]=0x3b, [kVK_F2]=0x3c, [kVK_F3]=0x3d, [kVK_F4]=0x3e,
    [kVK_F5]=0x3f, [kVK_F6]=0x40, [kVK_F7]=0x41, [kVK_F8]=0x42,
    [kVK_F9]=0x43, [kVK_F10]=0x44, [kVK_F11]=0x57, [kVK_F12]=0x58,
    [kVK_F13]=0x64, [kVK_F14]=0x65, [kVK_F15]=0x66, [kVK_F16]=0x67,
    [kVK_F17]=0x68, [kVK_F18]=0x69, [kVK_F19]=0x6a, [kVK_F20]=0x6b,
    [kVK_Help]=0xe052, [kVK_Home]=0xe047, [kVK_End]=0xe04f,
    [kVK_PageUp]=0xe049, [kVK_PageDown]=0xe051, [kVK_ForwardDelete]=0xe053,
    [kVK_LeftArrow]=0xe04b, [kVK_RightArrow]=0xe04d,
    [kVK_UpArrow]=0xe048, [kVK_DownArrow]=0xe050,
    [kVK_JIS_Yen]=0x7d, [kVK_JIS_Underscore]=0x73,
    [kVK_JIS_Kana]=0xe0f2, [kVK_JIS_Eisu]=0xe0f1
};

int asb_mac_key_code(const InputPacket *packet)
{
    if (packet->type!=INPUT_KEY_PHYSICAL || !packet->param2 || packet->param2>255 ||
        (packet->param3&~(INPUT_KEY_EXTENDED|INPUT_KEY_UP))) return -1;
    uint16_t scan=(uint16_t)packet->param2 | ((packet->param3&INPUT_KEY_EXTENDED)?0xe000:0);
    for (unsigned key=0;key<128;++key) if (asb_mac_scan_codes[key]==scan) return (int)key;
    return -1;
}

static uint32_t text_packets_main(uint32_t scalar,InputPacket packets[8],unsigned *count)
{
    *count=0;uint16_t scan=0;unsigned modifiers=0;
    if (scalar=='\n' || scalar=='\r') scan=0x1c;
    else if (scalar=='\t') scan=0x0f;
    else if (scalar<=0xffff && !(scalar>=0xd800 && scalar<=0xdfff)) {
        TISInputSourceRef source=TISCopyCurrentKeyboardLayoutInputSource();
        CFDataRef data=source?TISGetInputSourceProperty(source,kTISPropertyUnicodeKeyLayoutData):NULL;
        if (!data) {
            if (source) CFRelease(source);
            source=TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
            data=source?TISGetInputSourceProperty(source,kTISPropertyUnicodeKeyLayoutData):NULL;
        }
        const UCKeyboardLayout *layout=data?(const UCKeyboardLayout *)CFDataGetBytePtr(data):NULL;
        for (unsigned variant=0;layout && !scan && variant<8;++variant) {
            UInt32 flags=((variant&1)?shiftKey:0)|((variant&2)?controlKey:0)|((variant&4)?optionKey:0);
            for (unsigned key=0;key<128;++key) {
                if (!asb_mac_scan_codes[key]) continue;
                UInt32 dead=0;UniChar value[4];UniCharCount length=0;
                OSStatus error=UCKeyTranslate(layout,(UInt16)key,kUCKeyActionDown,flags>>8,(UInt32)LMGetKbdType(),
                    kUCKeyTranslateNoDeadKeysMask,&dead,4,&length,value);
                if (!error && length==1 && value[0]==scalar) { scan=asb_mac_scan_codes[key];modifiers=variant;break; }
            }
        }
        if (source) CFRelease(source);
    }
    if (!scan) return 50;
    const unsigned keys[]={0x2a,0x1d,0x38};
    for (unsigned i=0;i<3;++i) if (modifiers&(1u<<i)) packets[(*count)++]=(InputPacket){INPUT_MAGIC,INPUT_KEY_PHYSICAL,0,keys[i],0};
    unsigned flags=(scan&0xff00)==0xe000?INPUT_KEY_EXTENDED:0;
    packets[(*count)++]=(InputPacket){INPUT_MAGIC,INPUT_KEY_PHYSICAL,0,scan&255,flags};
    packets[(*count)++]=(InputPacket){INPUT_MAGIC,INPUT_KEY_PHYSICAL,0,scan&255,flags|INPUT_KEY_UP};
    for (int i=2;i>=0;--i) if (modifiers&(1u<<i)) packets[(*count)++]=(InputPacket){INPUT_MAGIC,INPUT_KEY_PHYSICAL,0,keys[i],INPUT_KEY_UP};
    return 0;
}

typedef struct { uint32_t scalar;InputPacket *packets;unsigned *count;uint32_t error; } TextMapping;
static void map_text_on_main(void *context)
{
    TextMapping *mapping=context;
    mapping->error=text_packets_main(mapping->scalar,mapping->packets,mapping->count);
}
uint32_t asb_mac_text_packets(uint32_t scalar,InputPacket packets[8],unsigned *count)
{
    if (pthread_main_np()) return text_packets_main(scalar,packets,count);
    TextMapping mapping={scalar,packets,count,0};
    dispatch_sync_f(dispatch_get_main_queue(),&mapping,map_text_on_main);return mapping.error;
}

/* host.c - run/recompile host for Jelly Monsters (Commodore, 1981).
 *
 * Jelly Monsters is Commodore's VIC-20 Pac-Man clone - and a near-perfect static
 * recompilation target: an autostart cartridge that, at its cold-start vector,
 * pokes the VIC chip registers straight from a 16-byte table in its own ROM
 * ($A00F -> $9000), copies a custom 8x8 character set into RAM at $1400, and
 * draws its maze into screen RAM ($1C00) + colour RAM ($9400). No KERNAL ROM, no
 * BASIC, no CHARGEN dependency for the picture - the cartridge sets up the whole
 * machine itself, exactly the self-contained shape that makes Choplifter and the
 * Pac-Man arcade board clean targets.
 *
 * This host maps the cartridge onto the vic20recomp runtime and runs it from the
 * cold-start vector. It runs interpreted by default - the bring-up oracle - and,
 * once a recompiled image exists (generated/), dispatches recompiled functions
 * through the runtime table, interpreting the rest. KERNAL/BASIC space is filled
 * with RTS so the rare KERNAL call returns harmlessly; the VIC register defaults
 * a cold start would leave are pre-seeded for carts that expect them.
 *
 *   jellymonsters <cart.crt> [maxInsns] [--shot out.bmp] [--calls seeds.txt]
 *
 * env: VIC_HYBRID=1   dispatch the recompiled image (else pure interpreter)
 *      VIC_IRQ=<n>    fire the IRQ (via $0314 / $FFFE) every <n> instructions
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "vic20recomp/recomp_rt.h"
#include "vic20recomp/mem.h"
#include "vic20recomp/video.h"
#include "decode.h"
#if __has_include("recomp_funcs.h")
#  include "recomp_funcs.h"
#  define HAVE_RECOMP 1
#endif

/* --- VIC/VIA read model: the picture lives in flat RAM, but a few I/O reads
 * must progress so init loops that poll hardware terminate. The raster line and
 * the VIA timers tick; keyboard/joystick read idle (no key/stick). Writes are
 * stored to RAM by the runtime, so the VIC registers + colour RAM stay live. */
static unsigned g_raster, g_via;
static uint8_t io_hook(uint16_t a, int is_write, uint8_t val) {
    (void)val;
    if (is_write) return 0;                  /* store falls through to RAM */
    switch (a) {
        case 0x9004: return (uint8_t)(g_raster++ >> 1);     /* raster counter */
        case 0x9120: case 0x9121: return 0xFF;              /* keyboard: none */
        case 0x9110: case 0x9111: case 0x911F: return 0xFF; /* user/joy idle  */
        case 0x9114: case 0x9115: case 0x9118: case 0x9119:
        case 0x9124: case 0x9125: case 0x9128: case 0x9129:
            return (uint8_t)(g_via--);                      /* VIA timers tick */
        default: return vic_ram[a];
    }
}

/* --- interpreter over the shared vic_cpu / vic_mem (the bring-up oracle) --- */
static void push(uint8_t v) { vic_mem_write(0x0100 + vic_cpu.s, v); vic_cpu.s--; }
static uint8_t pull(void)   { vic_cpu.s++; return vic_mem_read(0x0100 + vic_cpu.s); }
static uint16_t ea(const insn_t *in) {
    switch (in->mode) {
        case AM_ZP:  return in->operand;
        case AM_ZPX: return (uint8_t)(in->operand + vic_cpu.x);
        case AM_ZPY: return (uint8_t)(in->operand + vic_cpu.y);
        case AM_ABS: return in->operand;
        case AM_ABX: return (uint16_t)(in->operand + vic_cpu.x);
        case AM_ABY: return (uint16_t)(in->operand + vic_cpu.y);
        case AM_IZX: return vic_zp_ptr((uint8_t)(in->operand + vic_cpu.x));
        case AM_IZY: return (uint16_t)(vic_zp_ptr((uint8_t)in->operand) + vic_cpu.y);
        default:     return 0;
    }
}
static uint8_t rdval(const insn_t *in) { return in->mode==AM_IMM ? (uint8_t)in->operand : vic_mem_read(ea(in)); }
#define MN(s) (!strcmp(in.mnemonic, s))
static long g_unhandled, g_icount;
static uint8_t g_calls[0x10000];          /* JSR/computed-jump targets (recompile seeds) */
static long g_ipage[16];
static int interp_one(void) {
    g_icount++;
    uint16_t pc = vic_cpu.pc;
    g_ipage[pc >> 12]++;
    uint8_t w[3] = { vic_mem_read(pc), vic_mem_read((uint16_t)(pc+1)), vic_mem_read((uint16_t)(pc+2)) };
    insn_t in; m6502_decode(w, pc, &in);
    vic_cpu.pc = (uint16_t)(pc + in.len);
    if (MN("LDA")) { vic_lda(rdval(&in)); return 0; }
    if (MN("LDX")) { vic_ldx(rdval(&in)); return 0; }
    if (MN("LDY")) { vic_ldy(rdval(&in)); return 0; }
    if (MN("STA")) { vic_mem_write(ea(&in), vic_cpu.a); return 0; }
    if (MN("STX")) { vic_mem_write(ea(&in), vic_cpu.x); return 0; }
    if (MN("STY")) { vic_mem_write(ea(&in), vic_cpu.y); return 0; }
    if (MN("ORA")) { vic_ora(rdval(&in)); return 0; }
    if (MN("AND")) { vic_and(rdval(&in)); return 0; }
    if (MN("EOR")) { vic_eor(rdval(&in)); return 0; }
    if (MN("ADC")) { vic_adc(rdval(&in)); return 0; }
    if (MN("SBC")) { vic_sbc(rdval(&in)); return 0; }
    if (MN("CMP")) { vic_cmp(rdval(&in)); return 0; }
    if (MN("CPX")) { vic_cpx(rdval(&in)); return 0; }
    if (MN("CPY")) { vic_cpy(rdval(&in)); return 0; }
    if (MN("BIT")) { vic_bit(rdval(&in)); return 0; }
    if (MN("ASL")||MN("LSR")||MN("ROL")||MN("ROR")) {
        uint8_t (*op)(uint8_t)=MN("ASL")?vic_alu_asl:MN("LSR")?vic_alu_lsr:MN("ROL")?vic_alu_rol:vic_alu_ror;
        if (in.mode==AM_ACC) vic_cpu.a=op(vic_cpu.a); else { uint16_t a=ea(&in); vic_mem_write(a,op(vic_mem_read(a))); }
        return 0;
    }
    if (MN("INC")) { uint16_t a=ea(&in); vic_mem_write(a,vic_alu_inc(vic_mem_read(a))); return 0; }
    if (MN("DEC")) { uint16_t a=ea(&in); vic_mem_write(a,vic_alu_dec(vic_mem_read(a))); return 0; }
    if (MN("INX")) { vic_inx(); return 0; }  if (MN("INY")) { vic_iny(); return 0; }
    if (MN("DEX")) { vic_dex(); return 0; }  if (MN("DEY")) { vic_dey(); return 0; }
    if (MN("TAX")) { vic_tax(); return 0; }  if (MN("TAY")) { vic_tay(); return 0; }
    if (MN("TXA")) { vic_txa(); return 0; }  if (MN("TYA")) { vic_tya(); return 0; }
    if (MN("TSX")) { vic_tsx(); return 0; }  if (MN("TXS")) { vic_txs(); return 0; }
    if (MN("PHA")) { vic_pha(); return 0; }  if (MN("PHP")) { vic_php(); return 0; }
    if (MN("PLA")) { vic_pla(); return 0; }  if (MN("PLP")) { vic_plp(); return 0; }
    if (MN("CLC")) { vic_cpu.c=0; return 0; } if (MN("SEC")) { vic_cpu.c=1; return 0; }
    if (MN("CLI")) { vic_cpu.i=0; return 0; } if (MN("SEI")) { vic_cpu.i=1; return 0; }
    if (MN("CLD")) { vic_cpu.d=0; return 0; } if (MN("SED")) { vic_cpu.d=1; return 0; }
    if (MN("CLV")) { vic_cpu.v=0; return 0; } if (MN("NOP")) { return 0; }
    if (in.cflow==CF_BRANCH) {
        int t=0;
        if (MN("BPL")) t=!vic_cpu.n; else if (MN("BMI")) t=vic_cpu.n;
        else if (MN("BVC")) t=!vic_cpu.v; else if (MN("BVS")) t=vic_cpu.v;
        else if (MN("BCC")) t=!vic_cpu.c; else if (MN("BCS")) t=vic_cpu.c;
        else if (MN("BNE")) t=!vic_cpu.z; else if (MN("BEQ")) t=vic_cpu.z;
        if (t) vic_cpu.pc=in.target;
        return 0;
    }
    if (MN("JMP")) {
        uint16_t t = in.mode==AM_IND ? vic_mem_read16_bug(in.operand) : in.target;
        if (in.mode==AM_IND) g_calls[t] = 1;   /* computed-jump target: a routine entry */
        vic_cpu.pc = t; return 0;
    }
    if (MN("JSR")) { g_calls[in.target]=1;
                     uint16_t r=(uint16_t)(pc+2); push((uint8_t)(r>>8)); push((uint8_t)(r&0xFF)); vic_cpu.pc=in.target; return 0; }
    if (MN("RTS")) { uint16_t lo=pull(),hi=pull(); vic_cpu.pc=(uint16_t)((lo|(hi<<8))+1); return 0; }
    if (MN("RTI")) { vic_plp(); uint16_t lo=pull(),hi=pull(); vic_cpu.pc=(uint16_t)(lo|(hi<<8)); return 0; }
    if (MN("BRK")) { vic_cpu.pc=vic_mem_read16(VIC_VEC_IRQ); return 0; }
    return -1;
}

/* --- hybrid dispatch + budget yield (used when a recompiled image is present) --- */
static jmp_buf g_jmp; static long g_budget; static int g_norecomp;
static long g_rcount;
static void step1(void);
#ifdef HAVE_RECOMP
static uint16_t stack_ret(void) { uint8_t s=vic_cpu.s;
    return (uint16_t)(vic_mem_read((uint16_t)(0x100+(uint8_t)(s+1)))|(vic_mem_read((uint16_t)(0x100+(uint8_t)(s+2)))<<8)); }
static void interp_until(uint16_t addr, uint16_t until) {
    vic_cpu.pc=addr; long g=0; while (vic_cpu.pc!=until && g++<80000000L) { if (interp_one()<0){ g_unhandled++; vic_cpu.pc++; } }
}
static void h_ext_call(uint16_t a){ if (vic_has_func(a)) { vic_call_addr(a); return; } interp_until(a,(uint16_t)(stack_ret()+1)); }
#endif
static int g_last_recomp;
static void step1(void) {
    if (--g_budget <= 0) longjmp(g_jmp, 1);
#ifdef HAVE_RECOMP
    uint16_t pc = vic_cpu.pc;
    if (!g_norecomp && vic_has_func(pc)) { g_rcount++; vic_call_addr(pc); g_last_recomp = 1; return; }
    if (!g_norecomp && g_last_recomp) { g_calls[vic_cpu.pc] = 1; g_last_recomp = 0; }
#endif
    if (interp_one() < 0) { g_unhandled++; vic_cpu.pc++; }
}

/* --- screen dump (ASCII, from screen RAM) for headless inspection --- */
static void render_ascii(void) {
    uint8_t r2=vic_ram[0x9002], r3=vic_ram[0x9003], r5=vic_ram[0x9005];
    int cols=r2&0x7F; if(cols<1||cols>32)cols=22;
    int rows=(r3>>1)&0x3F; if(rows<1||rows>32)rows=23;
    int sb=(r5>>4)&0xF; uint16_t scr=(uint16_t)((sb>=8?(sb-8)*0x400:0x8000+sb*0x400)+((r2&0x80)?0x200:0));
    printf("  --- screen $%04X (%dx%d) ---\n", scr, cols, rows);
    for (int y=0;y<rows;y++){ char l[40]; int w=0,last=-1;
        for (int x=0;x<cols && w<38;x++){ uint8_t c=vic_ram[(uint16_t)(scr+y*cols+x)]&0x7F;
            char ch=(c<32)?(char)(c+64):(char)c; if(ch<0x20||ch>0x7E)ch='.'; l[w]=ch; if(ch!=' ')last=w; w++; }
        l[last+1]='\0'; if(last>=0) printf("  |%s\n", l); }
}

/* Save the current VIC frame (vic_render_rgba) as a 24-bit BMP. */
static void save_bmp(const char *path) {
    static uint32_t fb[VIC_MAX_W * VIC_MAX_H];
    int w=0,h=0; vic_render_rgba(fb, &w, &h);
    int rowsz = (w*3 + 3) & ~3, datasz = rowsz*h, filesz = 54 + datasz;
    unsigned char hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=filesz; hdr[3]=filesz>>8; hdr[4]=filesz>>16; hdr[5]=filesz>>24;
    hdr[10]=54; hdr[14]=40; hdr[18]=w; hdr[19]=w>>8; hdr[22]=h; hdr[23]=h>>8;
    hdr[26]=1; hdr[28]=24;
    hdr[34]=datasz; hdr[35]=datasz>>8; hdr[36]=datasz>>16; hdr[37]=datasz>>24;
    FILE *f = fopen(path, "wb"); if (!f) return;
    fwrite(hdr, 1, 54, f);
    unsigned char *row = malloc(rowsz);
    for (int y = h-1; y >= 0; y--) {            /* BMP is bottom-up */
        memset(row, 0, rowsz);
        for (int x = 0; x < w; x++) { uint32_t p = fb[y*w+x];
            row[x*3+0]=p&0xFF; row[x*3+1]=(p>>8)&0xFF; row[x*3+2]=(p>>16)&0xFF; }
        fwrite(row, 1, rowsz, f);
    }
    free(row); fclose(f);
    printf("wrote screenshot (%dx%d) -> %s\n", w, h, path);
}

static uint8_t *read_file(const char *p, size_t *n) {
    FILE *f=fopen(p,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long s=ftell(f); fseek(f,0,SEEK_SET); if(s<=0){fclose(f);return NULL;}
    uint8_t *b=malloc((size_t)s); if(fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;}
    fclose(f); *n=(size_t)s; return b;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <cart.crt> [maxInsns] [--shot out.bmp] [--calls seeds.txt]\n"
                        "  Runs the cartridge (interpreted by default). VIC_HYBRID=1 dispatches the\n"
                        "  recompiled image; VIC_IRQ=<n> fires the IRQ every <n> instructions.\n", argv[0]);
        return 2;
    }
    size_t csz=0;
    uint8_t *cart = read_file(argv[1], &csz);
    if (!cart) { fprintf(stderr, "cannot read cart %s\n", argv[1]); return 1; }
    long maxi = (argc>2 && argv[2][0]!='-') ? strtol(argv[2], NULL, 0) : 20000000L;
    const char *shot=NULL, *callsfile=NULL;
    for (int i=2;i<argc;i++) {
        if (i<argc-1 && !strcmp(argv[i],"--shot"))  shot=argv[i+1];
        if (i<argc-1 && !strcmp(argv[i],"--calls")) callsfile=argv[i+1];
    }

    /* Strip a 2-byte PRG-style load-address header ($00 $A0) if present; map the
     * cartridge image at $A000 (autostart region). */
    size_t off = (csz > 2 && cart[0]==0x00 && cart[1]==0xA0) ? 2 : 0;
    vic_mem_init();
    for (size_t i = off; i < csz; i++) {
        uint16_t a = (uint16_t)(VIC_CART_BASE + (i - off));
        if (a >= VIC_CART_BASE) vic_ram[a] = cart[i];
        if (a == 0xBFFF) break;
    }
    vic_set_default_regs();
    /* Fill BASIC + KERNAL ROM space with RTS ($60) so the rare KERNAL call a cart
     * makes returns harmlessly, and point the hardware vectors at an RTS. */
    for (int a = 0xC000; a <= 0xFFFF; a++) vic_ram[a] = 0x60;
    vic_ram[0xFFFA]=0x00; vic_ram[0xFFFB]=0xE0;   /* NMI   -> $E000 (RTS) */
    vic_ram[0xFFFC]=0x00; vic_ram[0xFFFD]=0xE0;   /* RESET -> $E000       */
    vic_ram[0xFFFE]=0x00; vic_ram[0xFFFF]=0xE0;   /* IRQ   -> $E000       */
    vic_io_hook = io_hook;

    vic_cpu_reset();
    uint16_t cold = vic_mem_read16(VIC_CART_BASE);   /* cold-start vector */
    vic_cpu.pc = cold;
    vic_cpu.s = 0xFF;

    g_norecomp = getenv("VIC_HYBRID") == NULL;
    long irq_every = getenv("VIC_IRQ") ? strtol(getenv("VIC_IRQ"), NULL, 0) : 0;
#ifdef HAVE_RECOMP
    vic_dispatch_reset(); vic_recomp_register();
    vic_hook_ext_call = h_ext_call; vic_hook_ext_jmp = h_ext_call; vic_hook_jmp_indirect = h_ext_call;
    printf("Jelly Monsters - hybrid host (recompiled + interpreted)\n");
#else
    printf("Jelly Monsters - interpreter runner (bring-up oracle)\n");
#endif
    printf("  cart %zu bytes -> $A000; cold-start $%04X\n\n", csz-off, cold);

    g_budget = maxi;
    long fired = 0;
    if (setjmp(g_jmp) == 0) {
        for (;;) {
            step1();
            if (irq_every && (g_icount % irq_every) == 0 && !vic_cpu.i) {
                /* Deliver an IRQ through the RAM vector the cart installed at $0314
                 * (KERNAL convention), else the hardware $FFFE vector. */
                uint16_t vec = vic_mem_read16(VIC_VEC_CINV);
                if (vec < 0xA000 || vec >= 0xC000) vec = vic_mem_read16(VIC_VEC_IRQ);
                if (vec >= 0xA000 && vec < 0xC000) {
                    push((uint8_t)(vic_cpu.pc>>8)); push((uint8_t)(vic_cpu.pc&0xFF));
                    push((uint8_t)(vic_p_pack() & ~0x10)); vic_cpu.i=1; vic_cpu.pc=vec; fired++;
                }
            }
        }
    }

    printf("ran ~%ld instructions; unhandled opcodes: %ld; pc=$%04X\n",
           maxi - g_budget, g_unhandled, vic_cpu.pc);
    printf("  dispatch: %ld recompiled, %ld interpreted; irq fired: %ld\n",
           g_rcount, g_icount, fired);
    {   uint8_t r5=vic_ram[0x9005], r2=vic_ram[0x9002], rF=vic_ram[0x900F];
        printf("  VIC: $9002=%02X $9003=%02X $9005=%02X $900E=%02X $900F=%02X (bg=%d border=%d)\n",
               r2, vic_ram[0x9003], r5, vic_ram[0x900E], rF, (rF>>4)&0xF, rF&7);
    }
    printf("  lit (foreground) pixels: %ld\n", vic_lit_pixels());
    render_ascii();

    if (shot) save_bmp(shot);
    if (callsfile) { FILE *o=fopen(callsfile,"w"); if(o){ int n=0;
                     for (int a=0;a<0x10000;a++) if (g_calls[a]) { fprintf(o,"0x%04X\n",a); n++; }
                     /* always include the cold-start entry */
                     fprintf(o,"0x%04X\n", cold);
                     fclose(o); printf("wrote %d JSR-target seeds -> %s\n", n+1, callsfile); } }
    free(cart);
    return 0;
}

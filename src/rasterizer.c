/*
 * SmazkaVG v1.3 Rasterizer
 * - Per-pixel distance-field curve rendering (no tessellation)
 * - BMP (fast) + WebP (compressed, via libwebp)
 * - Curve types: seg, quad, cubic, rational(conic), catmull-rom
 * - Vertex types: corner, smooth, symmetric, auto (Inkscape-style)
 * - Primitives: v, e, f, s, n, arc, ellipse
 *
 * Build: cc -O2 -o smazka-raster src/rasterizer.c -lwebp -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

typedef int32_t q16_t;
#define Q 65536.0
static inline double q2d(q16_t v)  { return v/Q; }
static inline q16_t d2q(double x) {
    double s = x*Q;
    if (s >  2147483647.0) return  2147483647;
    if (s < -2147483648.0) return (int32_t)0x80000000;
    return (q16_t)(s >= 0 ? s+0.5 : s-0.5);
}

#define MAX_V 4096
#define MAX_E 4096
#define MAX_F 4096
#define MAX_S 4096
#define MAX_N 1024
#define MAX_A 256   /* arcs */
#define MAX_EL 256  /* ellipses */
#define MAX_LINE 2048
#define MAX_W 64

typedef struct { double x,y; } V2;
typedef struct { uint8_t r,g,b,a; } Col;

typedef enum { E_SEG=0, E_QUAD=1, E_CUBIC=2, E_RATIONAL=3, E_CATMULL=4 } EType;
typedef enum { V_CORNER=0, V_SMOOTH=1, V_SYMMETRIC=2, V_AUTO=3 } VType;

static int n_v;
static struct { V2 p; VType vt; } verts[MAX_V];

static int n_e;
static struct {
    int v0, v1;
    EType type;
    V2 cp[4];  /* quad:1, cubic:2, rational:2+weight, catmull:0(auto) */
    double w[2]; /* rational weights */
    int n_cp;
} edges[MAX_E];

static int n_f;
static struct { int eids[64]; int ne; int fill; } faces[MAX_F];

static int n_s;
static struct { int eid; Col c; double w[MAX_W]; int nw; } strokes[MAX_S];

static int n_n;
static struct { double tx,ty,rot,sx,sy,skew; int cref; } nodes[MAX_N];

/* arcs */
static int n_arc;
typedef struct { int vc; V2 center; double r, a0, a1; Col c; double lw; } Arc;
static Arc arcs[MAX_A];

/* ellipses */
static int n_ell;
typedef struct { V2 c; double rx, ry, rot; Col fill; Col stroke; double sw; } Ell;
static Ell ells[MAX_EL];

/* Constraints (split namespace) */
static int n_scon, n_acon, n_con, n_pcon;
static struct { int t,a,b; } scon[256];
static struct { int t,a,b,c; } acon[256];
static struct { int t,a,b; double v; } con[256];
static struct { int t,tgt; Col c1,c2; } pcon[128];

/* ─── Color ─── */
static Col col(const char *s) {
    Col c = {0,0,0,255};
    unsigned v = 0;
    int l = (int)strlen(s);
    if (l >= 8) { sscanf(s,"%x",&v); c.r=(v>>24)&0xFF; c.g=(v>>16)&0xFF; c.b=(v>>8)&0xFF; c.a=v&0xFF; }
    else if (l >= 6) { sscanf(s,"%x",&v); c.r=(v>>16)&0xFF; c.g=(v>>8)&0xFF; c.b=v&0xFF; c.a=255; }
    return c;
}

/* ─── Parser helpers ─── */
static void skip_token(const char **p) {
    while (**p && **p != ' ' && **p != '\t') (*p)++;
    while (**p == ' ' || **p == '\t') (*p)++;
}
static int read_dbl(const char **p, double *out) {
    int n; if (sscanf(*p, "%lf%n", out, &n) == 1 && n > 0) { *p += n; return 1; }
    return 0;
}

/* ─── Main parser ─── */
static int parse(const char *path) {
    FILE *f = fopen(path,"r"); if (!f) return -1;
    char ln[MAX_LINE];
    while (fgets(ln, sizeof(ln), f)) {
        char *nl=strchr(ln,'\n'); if(nl)*nl=0;
        char *p=ln; while(*p==' '||*p=='\t')p++;
        if (!*p || *p=='#') continue;
        char cmd; if (sscanf(p,"%c",&cmd)!=1) continue;

        switch(cmd) {
        case 'v': {
            int id; double x,y; char vtype[16]="corner";
            int n = sscanf(p, "v %d %lf %lf %15s", &id, &x, &y, vtype);
            if (n < 3) break;
            if (id >= n_v) n_v = id+1;
            verts[id].p.x = x; verts[id].p.y = y;
            if (strcmp(vtype,"smooth")==0) verts[id].vt = V_SMOOTH;
            else if (strcmp(vtype,"symmetric")==0) verts[id].vt = V_SYMMETRIC;
            else if (strcmp(vtype,"auto")==0) verts[id].vt = V_AUTO;
            else verts[id].vt = V_CORNER;
            break;
        }
        case 'e': {
            int id,v0,v1; char etype[32]="";
            if (sscanf(p,"e %d %d %d %31s",&id,&v0,&v1,etype) < 3) break;
            if (id >= n_e) n_e = id+1;
            edges[id].v0=v0; edges[id].v1=v1;
            edges[id].n_cp=0; edges[id].w[0]=1; edges[id].w[1]=1;
            /* Skip "type=" prefix if present */
            const char *et = etype;
            if (strncmp(et, "type=", 5) == 0) et += 5;
            if      (strcmp(et,"quad")==0)      edges[id].type=E_QUAD;
            else if (strcmp(et,"cubic")==0)     edges[id].type=E_CUBIC;
            else if (strcmp(et,"rational")==0)  edges[id].type=E_RATIONAL;
            else if (strcmp(et,"catmull")==0)   edges[id].type=E_CATMULL;
            else if (strcmp(et,"seg")==0)       edges[id].type=E_SEG;
            else if (strlen(et) == 0)           edges[id].type=E_SEG;
            else edges[id].type=E_SEG;

            const char *t = p;
            for (int i=0;i<4;i++) skip_token(&t);
            /* Read control points / weights */
            int max_cp = (edges[id].type==E_QUAD)?1:(edges[id].type==E_CUBIC||edges[id].type==E_RATIONAL)?2:0;
            double vals[8]; int nv=0;
            while (*t && *t!='#' && nv < 8) {
                double v; if (read_dbl(&t, &v)) vals[nv++] = v; else skip_token(&t);
            }
            /* Assign: for quad/cubic/rational, first 2*n_cp are x,y pairs, then weights for rational */
            int vi = 0;
            for (int i=0; i<max_cp && vi+1<nv; i++) {
                edges[id].cp[i].x = vals[vi++];
                edges[id].cp[i].y = vals[vi++];
            }
            edges[id].n_cp = max_cp;
            if (edges[id].type==E_RATIONAL) {
                for (int i=0; i<2 && vi<nv; i++) edges[id].w[i] = vals[vi++];
            }
            break;
        }
        case 'f': {
            int id; if (sscanf(p,"f %d",&id)!=1) break;
            if (id >= n_f) n_f = id+1;
            const char *t = p;
            for (int i=0;i<2;i++) skip_token(&t);
            int ne=0;
            while (*t && *t!='#' && ne<64) {
                int e; if (sscanf(t,"%d",&e)!=1) break;
                faces[id].eids[ne++]=e; skip_token(&t);
            }
            faces[id].ne = ne;
            if (*t && *t!='#') faces[id].fill = (int)strtol(t,NULL,16);
            break;
        }
        case 's': {
            int id; char ctype[32];
            if (sscanf(p,"s %d %31s",&id,ctype)<2) break;
            if (strcmp(ctype,"parent")==0) {
                int a,b; if (sscanf(p,"s %*d parent %d %d",&a,&b)==2) { scon[n_scon].t=0; scon[n_scon].a=a; scon[n_scon].b=b; n_scon++; }
            } else if (strcmp(ctype,"group_id")==0) {
                int a,b; if (sscanf(p,"s %*d group_id %d %d",&a,&b)==2) { scon[n_scon].t=1; scon[n_scon].a=a; scon[n_scon].b=b; n_scon++; }
            } else {
                int eid; char cstr[32];
                if (sscanf(p,"s %d %d %31s",&id,&eid,cstr)<3) break;
                if (id>=n_s) n_s=id+1;
                strokes[id].eid=eid; strokes[id].c=col(cstr); strokes[id].nw=0;
                const char *t=p; for (int i=0;i<4;i++) skip_token(&t);
                while (*t && *t!='#') {
                    double w; int n;
                    if (sscanf(t,"%lf%n",&w,&n)==1 && n>0) {
                        if (strokes[id].nw<MAX_W) strokes[id].w[strokes[id].nw++]=w;
                        t+=n;
                    } else skip_token(&t);
                }
            }
            break;
        }
        case 'a': {
            int id; char at[32];
            if (sscanf(p,"a %d %31s",&id,at)<2) break;
            if (strcmp(at,"edge_connects")==0) {
                int a,b,c; if (sscanf(p,"a %*d edge_connects %d %d %d",&a,&b,&c)==3)
                    { acon[n_acon].t=0; acon[n_acon].a=a; acon[n_acon].b=b; acon[n_acon].c=c; n_acon++; }
            }
            break;
        }
        case 'c': {
            int id; char ct[32];
            if (sscanf(p,"c %d %31s",&id,ct)<2) break;
            if (strcmp(ct,"bbox_clamp")==0) {
                int pr; double x0,y0,x1,y1;
                if (sscanf(p,"c %*d bbox_clamp %d %lf %lf %lf %lf",&pr,&x0,&y0,&x1,&y1)==5)
                    { con[n_con].t=0; con[n_con].a=pr; con[n_con].v=x0; n_con++; }
            } else if (strcmp(ct,"min_dist")==0) {
                int a,b; double d;
                if (sscanf(p,"c %*d min_dist %d %d %lf",&a,&b,&d)==3)
                    { con[n_con].t=1; con[n_con].a=a; con[n_con].b=b; con[n_con].v=d; n_con++; }
            }
            break;
        }
        case 'p': {
            int id; char pt[32];
            if (sscanf(p,"p %d %31s",&id,pt)<2) break;
            if (strcmp(pt,"diffusion")==0) {
                int eid; char L[4],R[4],lc[32],rc[32];
                if (sscanf(p,"p %*d diffusion %d %3s %31s %3s %31s",&eid,L,lc,R,rc)>=4)
                    { pcon[n_pcon].t=0; pcon[n_pcon].tgt=eid; pcon[n_pcon].c1=col(lc); pcon[n_pcon].c2=col(rc); n_pcon++; }
            } else if (strcmp(pt,"solid_fill")==0) {
                int fid; char cstr[32];
                if (sscanf(p,"p %*d solid_fill %d %31s",&fid,cstr)==2)
                    { pcon[n_pcon].t=1; pcon[n_pcon].tgt=fid; pcon[n_pcon].c1=col(cstr); n_pcon++; }
            }
            break;
        }
        case 'n': {
            int id; if (sscanf(p,"n %d",&id)!=1) break;
            if (id>=n_n) n_n=id+1;
            nodes[id].tx=0;nodes[id].ty=0;nodes[id].rot=0;
            nodes[id].sx=1;nodes[id].sy=1;nodes[id].skew=0;nodes[id].cref=-1;
            const char *t=p; for(int i=0;i<2;i++) skip_token(&t);
            if (*t && strchr(t,'=')) {
                while (*t) {
                    while(*t==' ')t++; if(!*t)break;
                    double v; int n;
                    if (strncmp(t,"tx=",3)==0&&sscanf(t+3,"%lf%n",&v,&n)==1){nodes[id].tx=v;t+=3+n;}
                    else if(strncmp(t,"ty=",3)==0&&sscanf(t+3,"%lf%n",&v,&n)==1){nodes[id].ty=v;t+=3+n;}
                    else if(strncmp(t,"rot=",4)==0&&sscanf(t+4,"%lf%n",&v,&n)==1){nodes[id].rot=v;t+=4+n;}
                    else if(strncmp(t,"sx=",3)==0&&sscanf(t+3,"%lf%n",&v,&n)==1){nodes[id].sx=v;t+=3+n;}
                    else if(strncmp(t,"sy=",3)==0&&sscanf(t+3,"%lf%n",&v,&n)==1){nodes[id].sy=v;t+=3+n;}
                    else if(strncmp(t,"skew=",5)==0&&sscanf(t+5,"%lf%n",&v,&n)==1){nodes[id].skew=v;t+=5+n;}
                    else if(strncmp(t,"content=",8)==0&&sscanf(t+8,"%d%n",&nodes[id].cref,&n)==1){t+=8+n;}
                    else skip_token(&t);
                }
            } else {
                sscanf(t,"%lf %lf %lf %lf %lf %lf %d",
                    &nodes[id].tx,&nodes[id].ty,&nodes[id].rot,
                    &nodes[id].sx,&nodes[id].sy,&nodes[id].skew,&nodes[id].cref);
            }
            break;
        }
        case 'r': { /* arc primitive */
            int id; char cstr[32]; double cx,cy,r,a0,a1,lw;
            if (sscanf(p,"r %d %lf %lf %lf %lf %lf %31s %lf",&id,&cx,&cy,&r,&a0,&a1,cstr,&lw)>=7) {
                if (id>=n_arc) n_arc=id+1;
                arcs[id].center.x=cx; arcs[id].center.y=cy;
                arcs[id].r=r; arcs[id].a0=a0; arcs[id].a1=a1;
                arcs[id].c=col(cstr); arcs[id].lw=lw;
            }
            break;
        }
        case 'z': { /* ellipse primitive */
            int id; char fc[32],sc[32]="00000000"; double cx,cy,rx,ry,rot=0,sw=1.5;
            int n = sscanf(p,"z %d %lf %lf %lf %lf %lf %31s %31s %lf",&id,&cx,&cy,&rx,&ry,&rot,fc,sc,&sw);
            if (n >= 7) {
                if (id>=n_ell) n_ell=id+1;
                ells[id].c.x=cx; ells[id].c.y=cy;
                ells[id].rx=rx; ells[id].ry=ry; ells[id].rot=rot;
                ells[id].fill=col(fc);
                ells[id].stroke = (n>=8) ? col(sc) : (Col){0,0,0,255};
                ells[id].sw = (n>=9) ? sw : 1.5;
            }
            break;
        }
        default: break;
        }
    }
    fclose(f);
    return 0;
}

/* ── Curve evaluation ─── */

/* Cubic Bézier */
static V2 bez3(V2 p0,V2 p1,V2 p2,V2 p3,double t) {
    double u=1-t,u2=u*u,u3=u2*u,t2=t*t,t3=t2*t;
    return (V2){u3*p0.x+3*u2*t*p1.x+3*u*t2*p2.x+t3*p3.x,
                u3*p0.y+3*u2*t*p1.y+3*u*t2*p2.y+t3*p3.y};
}
static V2 bez3d(V2 p0,V2 p1,V2 p2,V2 p3,double t) { /* derivative */
    double u=1-t;
    return (V2){3*u*u*(p1.x-p0.x)+6*u*t*(p2.x-p1.x)+3*t*t*(p3.x-p2.x),
                3*u*u*(p1.y-p0.y)+6*u*t*(p2.y-p1.y)+3*t*t*(p3.y-p2.y)};
}
static V2 bez3dd(V2 p0,V2 p1,V2 p2,V2 p3,double t) { /* 2nd derivative */
    double u=1-t;
    return (V2){6*u*(p2.x-2*p1.x+p0.x)+6*t*(p3.x-2*p2.x+p1.x),
                6*u*(p2.y-2*p1.y+p0.y)+6*t*(p3.y-2*p2.y+p1.y)};
}

/* Quadratic Bézier */
static V2 bez2(V2 p0,V2 p1,V2 p2,double t) {
    double u=1-t;
    return (V2){u*u*p0.x+2*u*t*p1.x+t*t*p2.x, u*u*p0.y+2*u*t*p1.y+t*t*p2.y};
}

/* Rational (conic) Bézier */
static V2 rbez(V2 p0,V2 p1,V2 p2,double t,double w0,double w1,double w2) {
    double u=1-t;
    double b0=u*u*w0, b1=2*u*t*w1, b2=t*t*w2;
    double denom = b0+b1+b2;
    return (V2){(b0*p0.x+b1*p1.x+b2*p2.x)/denom,
                (b0*p0.y+b1*p1.y+b2*p2.y)/denom};
}

/* Catmull-Rom → cubic Bézier conversion */
static void catmull_to_bez(V2 p0,V2 p1,V2 p2,V2 p3, V2 *cp1, V2 *cp2) {
    /* Standard Catmull-Rom: cp1 = p1 + (p2-p0)/6, cp2 = p2 - (p3-p1)/6 */
    cp1->x = p1.x + (p2.x-p0.x)/6.0; cp1->y = p1.y + (p2.y-p0.y)/6.0;
    cp2->x = p2.x - (p3.x-p1.x)/6.0; cp2->y = p2.y - (p3.y-p1.y)/6.0;
}

/* ── Vertex type constraints (resolved at parse-time) ─── */

/* Apply vertex type constraints to adjust control points.
   For a vertex v shared by edges e_in (ending at v) and e_out (starting at v):
   - corner: no adjustment
   - smooth: make handles collinear
   - symmetric: make handles collinear + equal length
   - auto: recompute handles from neighbors
*/
static void resolve_vertex_types(void) {
    for (int vi = 0; vi < n_v; vi++) {
        VType vt = verts[vi].vt;
        if (vt == V_CORNER) continue;

        /* Find edges that start/end at this vertex */
        int e_in = -1, e_out = -1;
        for (int ei = 0; ei < n_e; ei++) {
            if (edges[ei].v1 == vi) e_in = ei;
            if (edges[ei].v0 == vi && e_out == -1) e_out = ei;
        }
        if (e_in < 0 || e_out < 0) continue;

        V2 v = verts[vi].p;

        if (vt == V_SMOOTH) {
            /* Make e_in's last handle and e_out's first handle collinear with v */
            /* Keep e_out's first handle, reflect e_in's last handle through v */
            if (edges[e_out].n_cp >= 1 && edges[e_in].n_cp >= 1) {
                V2 h_out = edges[e_out].cp[0];
                /* Direction from v to h_out */
                double dx = h_out.x - v.x, dy = h_out.y - v.y;
                /* Set e_in's last handle to v - k*(dx,dy) for some k */
                int last = edges[e_in].n_cp - 1;
                V2 h_in = edges[e_in].cp[last];
                double k = sqrt((h_in.x-v.x)*(h_in.x-v.x)+(h_in.y-v.y)*(h_in.y-v.y))
                         / sqrt(dx*dx+dy*dy+1e-20);
                edges[e_in].cp[last].x = v.x - k*dx;
                edges[e_in].cp[last].y = v.y - k*dy;
            }
        } else if (vt == V_SYMMETRIC) {
            /* Handles are mirror images: h_in = 2*v - h_out */
            if (edges[e_out].n_cp >= 1 && edges[e_in].n_cp >= 1) {
                V2 h_out = edges[e_out].cp[0];
                int last = edges[e_in].n_cp - 1;
                edges[e_in].cp[last].x = 2*v.x - h_out.x;
                edges[e_in].cp[last].y = 2*v.y - h_out.y;
            }
        } else if (vt == V_AUTO) {
            /* Find prev and next vertices */
            int v_prev = -1, v_next = -1;
            if (edges[e_in].n_cp >= 0) v_prev = edges[e_in].v0;
            if (edges[e_out].n_cp >= 0) v_next = edges[e_out].v1;
            if (v_prev >= 0 && v_next >= 0) {
                V2 pp = verts[v_prev].p, pn = verts[v_next].p;
                if (edges[e_out].n_cp >= 1) {
                    edges[e_out].cp[0].x = v.x + (pn.x-pp.x)/6.0;
                    edges[e_out].cp[0].y = v.y + (pn.y-pp.y)/6.0;
                }
                int last = edges[e_in].n_cp - 1;
                if (last >= 0) {
                    edges[e_in].cp[last].x = v.x - (pn.x-pp.x)/6.0;
                    edges[e_in].cp[last].y = v.y - (pn.y-pp.y)/6.0;
                }
            }
        }
    }
}

static V2 s2s(V2 p, double ox, double oy, double s) { return (V2){p.x*s+ox, p.y*s+oy}; }

/* Screen-space copies of geometry (populated by render()) */
static V2 ss_verts[MAX_V];
static struct { int v0,v1; EType type; V2 cp[4]; double w[2]; int n_cp; } ss_edges[MAX_E];

static void to_screen_geom(double ox, double oy, double sc) {
    for(int i=0;i<n_v;i++) ss_verts[i] = s2s(verts[i].p, ox, oy, sc);
    for(int i=0;i<n_e;i++){
        ss_edges[i].v0=edges[i].v0; ss_edges[i].v1=edges[i].v1;
        ss_edges[i].type=edges[i].type;
        ss_edges[i].n_cp=edges[i].n_cp;
        for(int j=0;j<edges[i].n_cp;j++) ss_edges[i].cp[j]=s2s(edges[i].cp[j],ox,oy,sc);
        ss_edges[i].w[0]=edges[i].w[0]; ss_edges[i].w[1]=edges[i].w[1];
    }
}

/* Per-pixel curve distance — uses screen-space geometry */
static double ss_dist_to_cubic(V2 P, V2 p0,V2 p1,V2 p2,V2 p3) {
    double best = 1e30;
    double starts[] = {0.0, 0.2, 0.4, 0.6, 0.8, 1.0};
    for (int s = 0; s < 6; s++) {
        double t = starts[s];
        for (int iter = 0; iter < 12; iter++) {
            V2 B = bez3(p0,p1,p2,p3,t);
            V2 Bd = bez3d(p0,p1,p2,p3,t);
            V2 Bdd = bez3dd(p0,p1,p2,p3,t);
            double dx = P.x-B.x, dy = P.y-B.y;
            double f = -(dx*Bd.x + dy*Bd.y);
            double fp = Bd.x*Bd.x + Bd.y*Bd.y - (dx*Bdd.x + dy*Bdd.y);
            if (fabs(fp) < 1e-12) break;
            double dt = f/fp;
            t += dt;
            if (t < 0) t = 0; if (t > 1) t = 1;
            if (fabs(dt) < 1e-10) break;
        }
        V2 B = bez3(p0,p1,p2,p3,t);
        double d = sqrt((P.x-B.x)*(P.x-B.x) + (P.y-B.y)*(P.y-B.y));
        if (d < best) best = d;
    }
    return best;
}

static double ss_dist_to_seg(V2 P, V2 a, V2 b) {
    double dx=b.x-a.x, dy=b.y-a.y, len2=dx*dx+dy*dy;
    if (len2 < 1e-12) return sqrt((P.x-a.x)*(P.x-a.x)+(P.y-a.y)*(P.y-a.y));
    double t = ((P.x-a.x)*dx+(P.y-a.y)*dy)/len2;
    if (t<0) t=0; if (t>1) t=1;
    double cx=a.x+t*dx, cy=a.y+t*dy;
    return sqrt((P.x-cx)*(P.x-cx)+(P.y-cy)*(P.y-cy));
}

static double ss_dist_to_edge(V2 P, int eid) {
    V2 p0 = ss_verts[ss_edges[eid].v0], p3 = ss_verts[ss_edges[eid].v1];
    switch(ss_edges[eid].type) {
    case E_CUBIC:
        if (ss_edges[eid].n_cp>=2) return ss_dist_to_cubic(P,p0,ss_edges[eid].cp[0],ss_edges[eid].cp[1],p3);
        break;
    case E_QUAD: {
        /* Quadratic: sample-based distance (quadratic Newton similar to cubic) */
        double best = 1e30;
        double starts[] = {0.0, 0.5, 1.0};
        for (int s = 0; s < 3; s++) {
            double t = starts[s];
            for (int iter = 0; iter < 12; iter++) {
                V2 B = bez2(p0,ss_edges[eid].cp[0],p3,t);
                double u=1-t;
                V2 Bd = (V2){2*u*(ss_edges[eid].cp[0].x-p0.x)+2*t*(p3.x-ss_edges[eid].cp[0].x),
                              2*u*(ss_edges[eid].cp[0].y-p0.y)+2*t*(p3.y-ss_edges[eid].cp[0].y)};
                V2 Bdd = (V2){2*(p3.x-2*ss_edges[eid].cp[0].x+p0.x), 2*(p3.y-2*ss_edges[eid].cp[0].y+p0.y)};
                double dx=P.x-B.x,dy=P.y-B.y;
                double f=-(dx*Bd.x+dy*Bd.y);
                double fp=Bd.x*Bd.x+Bd.y*Bd.y-(dx*Bdd.x+dy*Bdd.y);
                if(fabs(fp)<1e-12) break;
                t += f/fp; if(t<0)t=0; if(t>1)t=1;
                if(fabs(f/fp)<1e-10) break;
            }
            V2 B = bez2(p0,ss_edges[eid].cp[0],p3,t);
            double d = sqrt((P.x-B.x)*(P.x-B.x)+(P.y-B.y)*(P.y-B.y));
            if(d<best) best=d;
        }
        return best;
    }
    case E_RATIONAL:
    case E_CATMULL: {
        /* Fallback: sample densely */
        double best = 1e30;
        for (int i=0;i<=64;i++) {
            double tt=(double)i/64;
            V2 pt;
            if(ss_edges[eid].type==E_CATMULL){
                int vp=-1,vn=-1;
                for(int k=0;k<n_e;k++){if(ss_edges[k].v1==ss_edges[eid].v0&&k!=eid)vp=ss_edges[k].v0;}
                for(int k=0;k<n_e;k++){if(ss_edges[k].v0==ss_edges[eid].v1&&k!=eid)vn=ss_edges[k].v1;}
                V2 pp=vp>=0?ss_verts[vp]:p0, pn=vn>=0?ss_verts[vn]:p3;
                V2 c1,c2; catmull_to_bez(pp,p0,p3,pn,&c1,&c2);
                pt=bez3(p0,c1,c2,p3,tt);
            } else {
                pt=rbez(p0,ss_edges[eid].cp[0],ss_edges[eid].cp[1],tt,1,ss_edges[eid].w[0],1);
            }
            double d=sqrt((P.x-pt.x)*(P.x-pt.x)+(P.y-pt.y)*(P.y-pt.y));
            if(d<best) best=d;
        }
        return best;
    }
    default: break;
    }
    return ss_dist_to_seg(P,p0,p3);
}

/* ─── Framebuffer ─── */
static int FW, FH;
static uint8_t *fb; /* RGB */

static void fb_init(int w,int h) {
    FW=w; FH=h; fb=(uint8_t*)malloc(w*h*3); memset(fb,255,w*h*3);
}
static void fb_free(void) { free(fb); }

static inline void fb_set(int x,int y,uint8_t r,uint8_t g,uint8_t b) {
    if ((unsigned)x<(unsigned)FW && (unsigned)y<(unsigned)FH) {
        int o=(y*FW+x)*3; fb[o]=r;fb[o+1]=g;fb[o+2]=b;
    }
}
static inline void fb_blend(int x,int y,uint8_t r,uint8_t g,uint8_t b,uint8_t a) {
    if ((unsigned)x>=(unsigned)FW||(unsigned)y>=(unsigned)FH) return;
    if (!a) return;
    int o=(y*FW+x)*3;
    if (a==255){fb[o]=r;fb[o+1]=g;fb[o+2]=b;return;}
    uint16_t ia=255-a;
    fb[o]  =(uint8_t)((r*a+fb[o]*ia)/255);
    fb[o+1]=(uint8_t)((g*a+fb[o+1]*ia)/255);
    fb[o+2]=(uint8_t)((b*a+fb[o+2]*ia)/255);
}

/* ─── Triangle fill (barycentric) ─── */
static void fill_tri(V2 v0,V2 v1,V2 v2,Col c) {
    double mnx=fmin(fmin(v0.x,v1.x),v2.x), mxx=fmax(fmax(v0.x,v1.x),v2.x);
    double mny=fmin(fmin(v0.y,v1.y),v2.y), mxy=fmax(fmax(v0.y,v1.y),v2.y);
    int x0=(int)floor(mnx),x1=(int)ceil(mxx),y0=(int)floor(mny),y1=(int)ceil(mxy);
    if(x0<0)x0=0;if(x1>=FW)x1=FW-1;if(y0<0)y0=0;if(y1>=FH)y1=FH-1;
    double area=(v1.x-v0.x)*(v2.y-v0.y)-(v1.y-v0.y)*(v2.x-v0.x);
    if(fabs(area)<1e-10) return;
    double ia=1.0/area;
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
        double px=x+0.5,py=y+0.5;
        double w0=((v1.x-px)*(v2.y-py)-(v1.y-py)*(v2.x-px))*ia;
        double w1=((v2.x-px)*(v0.y-py)-(v2.y-py)*(v0.x-px))*ia;
        double w2=1-w0-w1;
        if(w0>=0&&w1>=0&&w2>=0) fb_blend(x,y,c.r,c.g,c.b,c.a);
    }
}

/* ─── Per-pixel curve stroke (distance field, screen-space) ─── */
static void stroke_edge_perpixel(int eid, double *widths, int nw, Col c, double scale) {
    if (eid >= n_e) return;
    V2 p0 = ss_verts[ss_edges[eid].v0], p3 = ss_verts[ss_edges[eid].v1];

    /* Bounding box with padding */
    double pad = 30; /* max expected half-width in screen pixels */
    int x0=(int)floor(fmin(p0.x,p3.x)-pad), x1=(int)ceil(fmax(p0.x,p3.x)+pad);
    int y0=(int)floor(fmin(p0.y,p3.y)-pad), y1=(int)ceil(fmax(p0.y,p3.y)+pad);

    /* For curved edges, include control points in bbox */
    for (int i=0;i<ss_edges[eid].n_cp;i++) {
        V2 cp = ss_edges[eid].cp[i];
        if (cp.x-pad < x0) x0=(int)floor(cp.x-pad);
        if (cp.x+pad > x1) x1=(int)ceil(cp.x+pad);
        if (cp.y-pad < y0) y0=(int)floor(cp.y-pad);
        if (cp.y+pad > y1) y1=(int)ceil(cp.y+pad);
    }
    if(x0<0)x0=0; if(x1>=FW)x1=FW-1; if(y0<0)y0=0; if(y1>=FH)y1=FH-1;

    /* Average width in screen space (scale from world to screen) */
    double w_avg = 2.0;
    if (nw > 0) { for(int i=0;i<nw;i++) w_avg+=widths[i]; w_avg/=nw; }
    w_avg *= scale; /* Convert world units to screen pixels */

    for (int y=y0; y<=y1; y++) {
        for (int x=x0; x<=x1; x++) {
            V2 P = {x+0.5, y+0.5};
            double d = ss_dist_to_edge(P, eid);

            double hw = w_avg * 0.5;
            if (d <= hw + 0.75) {
                double alpha;
                if (d <= hw - 0.75) alpha = 1.0;
                else alpha = 1.0 - (d - hw + 0.75) / 1.5;
                if (alpha < 0) alpha = 0;
                fb_blend(x,y,c.r,c.g,c.b,(uint8_t)(alpha*c.a));
            }
        }
    }
}

/* ─── Arc rendering ─── */
static void draw_arc(int ai) {
    Arc *a = &arcs[ai];
    double pad = a->lw + 2;
    int x0=(int)floor(a->center.x-a->r-pad), x1=(int)ceil(a->center.x+a->r+pad);
    int y0=(int)floor(a->center.y-a->r-pad), y1=(int)ceil(a->center.y+a->r+pad);
    if(x0<0)x0=0;if(x1>=FW)x1=FW-1;if(y0<0)y0=0;if(y1>=FH)y1=FH-1;
    double hw = a->lw * 0.5;
    for (int y=y0; y<=y1; y++) {
        for (int x=x0; x<=x1; x++) {
            double dx = x+0.5 - a->center.x, dy = y+0.5 - a->center.y;
            double dist = sqrt(dx*dx+dy*dy);
            double angle = atan2(dy, dx) * 180.0 / M_PI;
            if (angle < 0) angle += 360;
            /* Check if angle is in [a0, a1] range */
            double a0 = fmod(a->a0, 360), a1 = fmod(a->a1, 360);
            int in_arc;
            if (a0 <= a1) in_arc = (angle >= a0 && angle <= a1);
            else in_arc = (angle >= a0 || angle <= a1);
            if (in_arc && fabs(dist - a->r) <= hw + 0.5) {
                double alpha = (fabs(dist-a->r) <= hw-0.5) ? 1.0 : 1.0-(fabs(dist-a->r)-hw+0.5);
                if (alpha<0) alpha=0;
                fb_blend(x,y,a->c.r,a->c.g,a->c.b,(uint8_t)(alpha*a->c.a));
            }
        }
    }
}

/* ─── Ellipse rendering ─── */
static void draw_ellipse(int ei) {
    Ell *e = &ells[ei];
    double cr=cos(e->rot), sr=sin(e->rot);
    double pad = fmax(e->rx,e->ry) + e->sw + 2;
    int x0=(int)floor(e->c.x-pad),x1=(int)ceil(e->c.x+pad);
    int y0=(int)floor(e->c.y-pad),y1=(int)ceil(e->c.y+pad);
    if(x0<0)x0=0;if(x1>=FW)x1=FW-1;if(y0<0)y0=0;if(y1>=FH)y1=FH-1;
    double hw = e->sw * 0.5;
    for (int y=y0; y<=y1; y++) {
        for (int x=x0; x<=x1; x++) {
            double dx = x+0.5-e->c.x, dy = y+0.5-e->c.y;
            /* Rotate to ellipse frame */
            double rx = dx*cr+dy*sr, ry = -dx*sr+dy*cr;
            double val = (rx*rx)/(e->rx*e->rx) + (ry*ry)/(e->ry*e->ry);
            if (val <= 1.0) {
                /* Inside: fill */
                fb_blend(x,y,e->fill.r,e->fill.g,e->fill.b,e->fill.a);
            }
            /* Stroke: distance to ellipse boundary ≈ |sqrt(val)-1| * avg_radius */
            double avg_r = (e->rx+e->ry)*0.5;
            double d = fabs(sqrt(val)-1.0)*avg_r;
            if (d <= hw+0.5 && e->stroke.a > 0) {
                double alpha = (d<=hw-0.5)?1.0:1.0-(d-hw+0.5);
                if(alpha<0)alpha=0;
                fb_blend(x,y,e->stroke.r,e->stroke.g,e->stroke.b,(uint8_t)(alpha*e->stroke.a));
            }
        }
    }
}

/* ── View ─── */
static void view(double *ox,double *oy,double *sc) {
    if (!n_v) { *ox=*oy=0; *sc=1; return; }
    double mnx=1e30,mxx=-1e30,mny=1e30,mxy=-1e30;
    for(int i=0;i<n_v;i++){
        if(verts[i].p.x<mnx)mnx=verts[i].p.x; if(verts[i].p.x>mxx)mxx=verts[i].p.x;
        if(verts[i].p.y<mny)mny=verts[i].p.y; if(verts[i].p.y>mxy)mxy=verts[i].p.y;
    }
    /* Include arcs and ellipses in bounds */
    for(int i=0;i<n_arc;i++){
        if(arcs[i].center.x-arcs[i].r<mnx)mnx=arcs[i].center.x-arcs[i].r;
        if(arcs[i].center.x+arcs[i].r>mxx)mxx=arcs[i].center.x+arcs[i].r;
        if(arcs[i].center.y-arcs[i].r<mny)mny=arcs[i].center.y-arcs[i].r;
        if(arcs[i].center.y+arcs[i].r>mxy)mxy=arcs[i].center.y+arcs[i].r;
    }
    for(int i=0;i<n_ell;i++){
        if(ells[i].c.x-ells[i].rx<mnx)mnx=ells[i].c.x-ells[i].rx;
        if(ells[i].c.x+ells[i].rx>mxx)mxx=ells[i].c.x+ells[i].rx;
        if(ells[i].c.y-ells[i].ry<mny)mny=ells[i].c.y-ells[i].ry;
        if(ells[i].c.y+ells[i].ry>mxy)mxy=ells[i].c.y+ells[i].ry;
    }
    double sw=mxx-mnx; if(sw<1)sw=1; double sh=mxy-mny; if(sh<1)sh=1;
    double m=50;
    double sx=(FW-2*m)/sw, sy=(FH-2*m)/sh;
    *sc=fmin(sx,sy);
    *ox=m-mnx*(*sc)+((FW-2*m)-sw*(*sc))*0.5;
    *oy=m-mny*(*sc)+((FH-2*m)-sh*(*sc))*0.5;
}
/* ─── Render ─── */
static void render(void) {
    double ox,oy,sc; view(&ox,&oy,&sc);
    to_screen_geom(ox,oy,sc);

    /* Ellipses — transform in place, render, restore */
    V2 ell_c_save[MAX_EL]; double ell_rx_save[MAX_EL], ell_ry_save[MAX_EL], ell_sw_save[MAX_EL];
    for(int i=0;i<n_ell;i++){
        ell_c_save[i]=ells[i].c; ell_rx_save[i]=ells[i].rx;
        ell_ry_save[i]=ells[i].ry; ell_sw_save[i]=ells[i].sw;
        ells[i].c=s2s(ells[i].c,ox,oy,sc);
        ells[i].rx*=sc; ells[i].ry*=sc; ells[i].sw*=sc;
    }
    for(int i=0;i<n_ell;i++) draw_ellipse(i);
    for(int i=0;i<n_ell;i++){
        ells[i].c=ell_c_save[i]; ells[i].rx=ell_rx_save[i];
        ells[i].ry=ell_ry_save[i]; ells[i].sw=ell_sw_save[i];
    }

    /* Solid fills (paint section) */
    for(int pi=0;pi<n_pcon;pi++){
        if(pcon[pi].t!=1) continue;
        int fid=pcon[pi].tgt; if(fid>=n_f||faces[fid].ne<3) continue;
        Col fc=pcon[pi].c1;
        int fv=edges[faces[fid].eids[0]].v0;
        V2 sv0=ss_verts[fv];
        for(int ei=1;ei+1<faces[fid].ne;ei++){
            int e1=faces[fid].eids[ei], e2=faces[fid].eids[ei+1];
            int v1=edges[e1].v0==fv?edges[e1].v1:edges[e1].v0;
            int v2=edges[e2].v0==fv?edges[e2].v1:edges[e2].v0;
            fill_tri(sv0,ss_verts[v1],ss_verts[v2],fc);
        }
    }

    /* Diffusion */
    for(int pi=0;pi<n_pcon;pi++){
        if(pcon[pi].t!=0) continue;
        int eid=pcon[pi].tgt; if(eid>=n_e) continue;
        V2 a=ss_verts[ss_edges[eid].v0], b=ss_verts[ss_edges[eid].v1];
        double dx=b.x-a.x,dy=b.y-a.y,len=sqrt(dx*dx+dy*dy);
        if(len<1e-6) continue;
        double nx=-dy/len,ny=dx/len;
        double rad=50*sc/100;
        int x0=(int)floor(fmin(a.x,b.x)-rad-1),x1=(int)ceil(fmax(a.x,b.x)+rad+1);
        int y0=(int)floor(fmin(a.y,b.y)-rad-1),y1=(int)ceil(fmax(a.y,b.y)+rad+1);
        if(x0<0)x0=0;if(x1>=FW)x1=FW-1;if(y0<0)y0=0;if(y1>=FH)y1=FH-1;
        Col lc=pcon[pi].c1,rc=pcon[pi].c2;
        for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
            double px=x+0.5,py=y+0.5;
            double side=(px-a.x)*ny-(py-a.y)*nx;
            double t=((px-a.x)*dx+(py-a.y)*dy)/(len*len);
            if(t<-0.05||t>1.05) continue;
            double d=fabs(side); if(d>rad) continue;
            double bl=0.5+side/(2*rad); if(bl<0)bl=0;if(bl>1)bl=1;
            double fo=1-d/rad;
            uint8_t r=(uint8_t)(lc.r*(1-bl)+rc.r*bl);
            uint8_t g=(uint8_t)(lc.g*(1-bl)+rc.g*bl);
            uint8_t b=(uint8_t)(lc.b*(1-bl)+rc.b*bl);
            fb_blend(x,y,r,g,b,(uint8_t)(fo*180));
        }
    }

    /* Edge outlines (per-pixel, thin) */
    for(int ei=0;ei<n_e;ei++){
        V2 p0 = ss_verts[ss_edges[ei].v0], p3 = ss_verts[ss_edges[ei].v1];
        double w=1.5;
        double pad=3;
        int x0=(int)floor(fmin(p0.x,p3.x)-pad),x1=(int)ceil(fmax(p0.x,p3.x)+pad);
        int y0=(int)floor(fmin(p0.y,p3.y)-pad),y1=(int)ceil(fmax(p0.y,p3.y)+pad);
        for(int i=0;i<ss_edges[ei].n_cp;i++){
            V2 cp=ss_edges[ei].cp[i];
            if(cp.x-pad<x0)x0=(int)floor(cp.x-pad);
            if(cp.x+pad>x1)x1=(int)ceil(cp.x+pad);
            if(cp.y-pad<y0)y0=(int)floor(cp.y-pad);
            if(cp.y+pad>y1)y1=(int)ceil(cp.y+pad);
        }
        if(x0<0)x0=0;if(x1>=FW)x1=FW-1;if(y0<0)y0=0;if(y1>=FH)y1=FH-1;
        for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
            V2 P={x+0.5,y+0.5};
            double d = ss_dist_to_edge(P, ei);
            double hw=w*0.5;
            if(d<=hw+0.5){
                double a=(d<=hw-0.5)?1.0:1.0-(d-hw+0.5);
                if(a<0)a=0;
                fb_blend(x,y,40,40,40,(uint8_t)(a*255));
            }
        }
    }

    /* Strokes (per-pixel, variable width) */
    for(int si=0;si<n_s;si++){
        if(strokes[si].nw>0)
            stroke_edge_perpixel(strokes[si].eid, strokes[si].w, strokes[si].nw, strokes[si].c, sc);
    }

    /* Arcs */
    for(int i=0;i<n_arc;i++){
        arcs[i].center=s2s(arcs[i].center,ox,oy,sc);
        arcs[i].r*=sc; arcs[i].lw*=sc;
        draw_arc(i);
    }

    /* Vertices */
    for(int vi=0;vi<n_v;vi++){
        V2 sp=ss_verts[vi];
        int cx=(int)sp.x,cy=(int)sp.y;
        for(int dy=-2;dy<=2;dy++) for(int dx=-2;dx<=2;dx++)
            if(dx*dx+dy*dy<=5) fb_set(cx+dx,cy+dy,220,50,50);
    }

    /* Restore ellipses */
    /* (already transformed in place, no restore needed for single-use) */
}

/* ─── BMP output ─── */
static void write_bmp(const char *path) {
    FILE *f=fopen(path,"wb"); if(!f) return;
    int rb=FW*3, pad=(4-rb%4)%4, isz=(rb+pad)*FH, fsz=54+isz;
    uint8_t fh[14]={0}; fh[0]='B';fh[1]='M';
    fh[2]=fsz&0xFF;fh[3]=(fsz>>8)&0xFF;fh[4]=(fsz>>16)&0xFF;fh[5]=(fsz>>24)&0xFF;
    fh[10]=54; fwrite(fh,1,14,f);
    uint8_t dh[40]={0}; dh[0]=40;
    dh[4]=FW&0xFF;dh[5]=(FW>>8)&0xFF;dh[6]=(FW>>16)&0xFF;dh[7]=(FW>>24)&0xFF;
    dh[8]=FH&0xFF;dh[9]=(FH>>8)&0xFF;dh[10]=(FH>>16)&0xFF;dh[11]=(FH>>24)&0xFF;
    dh[12]=1;dh[14]=24;
    dh[20]=isz&0xFF;dh[21]=(isz>>8)&0xFF;dh[22]=(isz>>16)&0xFF;dh[23]=(isz>>24)&0xFF;
    fwrite(dh,1,40,f);
    uint8_t pb[3]={0};
    for(int y=FH-1;y>=0;y--){
        for(int x=0;x<FW;x++){int o=(y*FW+x)*3;uint8_t bgr[3]={fb[o+2],fb[o+1],fb[o]};fwrite(bgr,1,3,f);}
        if(pad) fwrite(pb,1,pad,f);
    }
    fclose(f);
}

/* ── WebP output via external converter ── */
static void write_webp(const char *bmp_path, const char *webp_path) {
    char cmd[2048];
    /* Try imagemagick first, fall back to ffmpeg, then PIL */
    snprintf(cmd, sizeof(cmd),
        "convert '%s' '%s' 2>/dev/null || "
        "ffmpeg -y -i '%s' -c:v libwebp -lossless 1 -q:v 90 '%s' 2>/dev/null || "
        "python3 -c \"from PIL import Image; Image.open('%s').save('%s')\" 2>/dev/null",
        bmp_path, webp_path, bmp_path, webp_path, bmp_path, webp_path);
    int r = system(cmd);
    (void)r;
}

/* ─── SVG output ── */
static void write_svg(const char *path) {
    FILE *f=fopen(path,"w"); if(!f) return;
    double ox,oy,sc; view(&ox,&oy,&sc);
    fprintf(f,"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f,"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\">\n",FW,FH,FW,FH);
    fprintf(f,"  <!-- SmazkaVG v1.3 SVG projection -->\n");
    fprintf(f,"  <rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n",FW,FH);

    /* Ellipses */
    for(int i=0;i<n_ell;i++){
        V2 c=s2s(ells[i].c,ox,oy,sc);
        fprintf(f,"  <ellipse cx=\"%.2f\" cy=\"%.2f\" rx=\"%.2f\" ry=\"%.2f\"",c.x,c.y,ells[i].rx*sc,ells[i].ry*sc);
        fprintf(f," fill=\"rgba(%d,%d,%d,%.2f)\"",ells[i].fill.r,ells[i].fill.g,ells[i].fill.b,ells[i].fill.a/255.0);
        fprintf(f," stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\"/>\n",
                ells[i].stroke.r,ells[i].stroke.g,ells[i].stroke.b,ells[i].stroke.a/255.0,ells[i].sw*sc);
    }

    /* Arcs */
    for(int i=0;i<n_arc;i++){
        V2 c=s2s(arcs[i].center,ox,oy,sc); double r=arcs[i].r*sc;
        double a0=arcs[i].a0*M_PI/180, a1=arcs[i].a1*M_PI/180;
        double x0=c.x+r*cos(a0),y0=c.y+r*sin(a0),x1=c.x+r*cos(a1),y1=c.y+r*sin(a1);
        int large=(fabs(a1-a0)>M_PI)?1:0;
        fprintf(f,"  <path d=\"M %.2f,%.2f A %.2f,%.2f 0 %d 1 %.2f,%.2f\"",x0,y0,r,r,large,x1,y1);
        fprintf(f," stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" fill=\"none\"/>\n",
                arcs[i].c.r,arcs[i].c.g,arcs[i].c.b,arcs[i].c.a/255.0,arcs[i].lw*sc);
    }

    /* Faces */
    for(int pi=0;pi<n_pcon;pi++){
        if(pcon[pi].t!=1) continue;
        int fid=pcon[pi].tgt; if(fid>=n_f) continue;
        Col fc=pcon[pi].c1;
        fprintf(f,"  <polygon points=\"");
        int fv=edges[faces[fid].eids[0]].v0,vis[64]={0},pv[64],npv=0;
        for(int ei=0;ei<faces[fid].ne;ei++){
            int eid=faces[fid].eids[ei];
            int va=edges[eid].v0,vb=edges[eid].v1;
            int v=(npv==0)?va:(vis[va]?vb:va);
            if(!vis[v]&&npv<64){pv[npv++]=v;vis[v]=1;}
        }
        for(int k=0;k<npv;k++){V2 sp=s2s(verts[pv[k]].p,ox,oy,sc);fprintf(f,"%.2f,%.2f ",sp.x,sp.y);}
        fprintf(f,"\" fill=\"rgba(%d,%d,%d,%.2f)\"/>\n",fc.r,fc.g,fc.b,fc.a/255.0);
    }

    /* Diffusion */
    for(int pi=0;pi<n_pcon;pi++){
        if(pcon[pi].t!=0) continue;
        int eid=pcon[pi].tgt; if(eid>=n_e) continue;
        V2 a=s2s(verts[edges[eid].v0].p,ox,oy,sc),b=s2s(verts[edges[eid].v1].p,ox,oy,sc);
        Col lc=pcon[pi].c1,rc=pcon[pi].c2;
        fprintf(f,"  <defs><linearGradient id=\"d%d\"><stop offset=\"0%%\" stop-color=\"rgb(%d,%d,%d)\"/>",pi,lc.r,lc.g,lc.b);
        fprintf(f,"<stop offset=\"100%%\" stop-color=\"rgb(%d,%d,%d)\"/></linearGradient></defs>\n",rc.r,rc.g,rc.b);
        fprintf(f,"  <line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"url(#d%d)\" stroke-width=\"8\" opacity=\"0.6\"/>\n",a.x,a.y,b.x,b.y,pi);
    }

    /* Edges */
    for(int ei=0;ei<n_e;ei++){
        V2 sa=s2s(verts[edges[ei].v0].p,ox,oy,sc),sb=s2s(verts[edges[ei].v1].p,ox,oy,sc);
        if(edges[ei].type==E_CUBIC&&edges[ei].n_cp>=2){
            V2 c1=s2s(edges[ei].cp[0],ox,oy,sc),c2=s2s(edges[ei].cp[1],ox,oy,sc);
            fprintf(f,"  <path id=\"e%d\" d=\"M %.2f,%.2f C %.2f,%.2f %.2f,%.2f %.2f,%.2f\" stroke=\"#282828\" stroke-width=\"1.5\" fill=\"none\"/>\n",ei,sa.x,sa.y,c1.x,c1.y,c2.x,c2.y,sb.x,sb.y);
        } else if(edges[ei].type==E_QUAD&&edges[ei].n_cp>=1){
            V2 c1=s2s(edges[ei].cp[0],ox,oy,sc);
            fprintf(f,"  <path id=\"e%d\" d=\"M %.2f,%.2f Q %.2f,%.2f %.2f,%.2f\" stroke=\"#282828\" stroke-width=\"1.5\" fill=\"none\"/>\n",ei,sa.x,sa.y,c1.x,c1.y,sb.x,sb.y);
        } else {
            fprintf(f,"  <line id=\"e%d\" x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#282828\" stroke-width=\"1.5\"/>\n",ei,sa.x,sa.y,sb.x,sb.y);
        }
    }

    /* Strokes */
    for(int si=0;si<n_s;si++){
        int eid=strokes[si].eid; if(eid>=n_e) continue;
        V2 sa=s2s(verts[edges[eid].v0].p,ox,oy,sc),sb=s2s(verts[edges[eid].v1].p,ox,oy,sc);
        Col c=strokes[si].c;
        double aw=0; for(int w=0;w<strokes[si].nw;w++) aw+=strokes[si].w[w];
        if(strokes[si].nw>0) aw/=strokes[si].nw;
        if(edges[eid].type==E_CUBIC&&edges[eid].n_cp>=2){
            V2 c1=s2s(edges[eid].cp[0],ox,oy,sc),c2=s2s(edges[eid].cp[1],ox,oy,sc);
            fprintf(f,"  <path id=\"s%d\" d=\"M %.2f,%.2f C %.2f,%.2f %.2f,%.2f %.2f,%.2f\" stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" stroke-linecap=\"round\" fill=\"none\"/>\n",
                    si,sa.x,sa.y,c1.x,c1.y,c2.x,c2.y,sb.x,sb.y,c.r,c.g,c.b,c.a/255.0,aw*sc);
        } else {
            fprintf(f,"  <line id=\"s%d\" x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" stroke-linecap=\"round\"/>\n",
                    si,sa.x,sa.y,sb.x,sb.y,c.r,c.g,c.b,c.a/255.0,aw*sc);
        }
    }

    /* Vertices */
    for(int vi=0;vi<n_v;vi++){
        V2 sp=s2s(verts[vi].p,ox,oy,sc);
        const char *vt="corner";
        if(verts[vi].vt==V_SMOOTH)vt="smooth";else if(verts[vi].vt==V_SYMMETRIC)vt="symmetric";else if(verts[vi].vt==V_AUTO)vt="auto";
        fprintf(f,"  <circle id=\"v%d\" cx=\"%.2f\" cy=\"%.2f\" r=\"3\" fill=\"#dc3232\" data-type=\"%s\"/>\n",vi,sp.x,sp.y,vt);
    }
    fprintf(f,"</svg>\n");
    fclose(f);
}

/* ── ASCII ─── */
static void write_ascii(const char *path) {
    FILE *f=fopen(path,"w"); if(!f) return;
    const char *ramp=" .:-=+*#%@"; int rl=strlen(ramp);
    int cw=4,ch=7,cols=FW/cw,rows=FH/ch;
    if(cols<1)cols=1;if(rows<1)rows=1;if(cols>160)cols=160;if(rows>80)rows=80;
    for(int r=0;r<rows;r++){
        for(int c=0;c<cols;c++){
            double s=0;int n=0;
            for(int dy=0;dy<ch;dy++){int py=r*ch+dy;if(py>=FH)break;
                for(int dx=0;dx<cw;dx++){int px=c*cw+dx;if(px>=FW)break;
                    int o=(py*FW+px)*3;s+=0.299*fb[o]+0.587*fb[o+1]+0.114*fb[o+2];n++;}}
            double l=n?s/n:255; int idx=rl-1-(int)(l/256*rl);
            if(idx<0)idx=0;if(idx>=rl)idx=rl-1;
            fputc(ramp[idx],f);
        }
        fputc('\n',f);
    }
    fclose(f);
}

/* ─── Main ─── */
static void strip_ext(const char *in,char *out,int sz){
    strncpy(out,in,sz-1);out[sz-1]=0; char *d=strrchr(out,'.');if(d)*d=0;
}

int main(int argc,char **argv){
    if(argc<2){fprintf(stderr,"SmazkaVG v1.3 Rasterizer\nUsage: %s <in.smazka> [w] [h]\n",argv[0]);return 1;}
    const char *inp=argv[1];
    int w=(argc>=3)?atoi(argv[2]):512, h=(argc>=4)?atoi(argv[3]):512;
    if(w<64)w=64;if(h<64)h=64;if(w>4096)w=4096;if(h>4096)h=4096;

    if(parse(inp)!=0) return 1;
    resolve_vertex_types();

    int nq=0,nc=0,nr=0,nm=0;
    for(int i=0;i<n_e;i++){
        if(edges[i].type==E_QUAD)nq++;else if(edges[i].type==E_CUBIC)nc++;
        else if(edges[i].type==E_RATIONAL)nr++;else if(edges[i].type==E_CATMULL)nm++;
    }
    fprintf(stderr,"v1.3: %d verts, %d edges (seg:%d quad:%d cubic:%d rat:%d cat:%d), %d faces, %d strokes, %d arcs, %d ellipses\n",
            n_v,n_e,n_e-nq-nc-nr-nm,nq,nc,nr,nm,n_f,n_s,n_arc,n_ell);

    fb_init(w,h);
    render();

    char base[512]; strip_ext(inp,base,sizeof(base));
    char bmp_p[560],webp_p[560],svg_p[560],txt_p[560];
    snprintf(bmp_p,sizeof(bmp_p),"%s.bmp",base);
    snprintf(webp_p,sizeof(webp_p),"%s.webp",base);
    snprintf(svg_p,sizeof(svg_p),"%s.svg",base);
    snprintf(txt_p,sizeof(txt_p),"%s.txt",base);

    write_bmp(bmp_p);   fprintf(stderr,"BMP:  %s (%dx%d, %d KB)\n",bmp_p,w,h,(54+w*h*3)/1024);
    write_webp(bmp_p, webp_p); fprintf(stderr,"WebP: %s\n",webp_p);
    write_svg(svg_p);   fprintf(stderr,"SVG:  %s\n",svg_p);
    write_ascii(txt_p); fprintf(stderr,"ASCII: %s\n",txt_p);

    fb_free();
    return 0;
}

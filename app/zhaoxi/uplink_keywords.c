/* SPDX-License-Identifier: Apache-2.0 */
/* Personal MFCC/DTW experiment for two explicit network choices. A calibrated
 * model is not a general speech recognizer or an accuracy guarantee. */
#include "uplink_keywords.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define FFT_SIZE 512u
#define FRAME_SIZE 400u
#define HOP_SIZE 160u
#define MEL_BANDS 26u
#define PI_F 3.14159265358979323846f

struct dsp_workspace {
    float real[FFT_SIZE],imag[FFT_SIZE],window[FRAME_SIZE];
    float filters[MEL_BANDS][FFT_SIZE/2+1];
    float dct[UPLINK_KW_COEFFS][MEL_BANDS];
    float energy[UPLINK_KW_MAX_FRAMES];
};
static int features_valid(const struct uplink_kw_features *f)
{
    if(!f || f->frames<8 || f->frames>UPLINK_KW_MAX_FRAMES)return 0;
    for(uint32_t i=0;i<f->frames;i++)for(unsigned j=0;j<UPLINK_KW_COEFFS;j++)
        if(!isfinite(f->values[i][j]) || fabsf(f->values[i][j])>10000.0f)return 0;
    return 1;
}
static void fft(float *real,float *imag)
{
    for(unsigned i=1,j=0;i<FFT_SIZE;i++) {
        unsigned bit=FFT_SIZE>>1;
        for(;j&bit;bit>>=1)j^=bit;
        j^=bit;
        if(i<j){float t=real[i];real[i]=real[j];real[j]=t;t=imag[i];imag[i]=imag[j];imag[j]=t;}
    }
    for(unsigned length=2;length<=FFT_SIZE;length<<=1) {
        float angle=-2.0f*PI_F/(float)length;
        float step_real=cosf(angle),step_imag=sinf(angle);
        for(unsigned start=0;start<FFT_SIZE;start+=length) {
            float wr=1.0f,wi=0.0f;
            for(unsigned j=0;j<length/2;j++) {
                unsigned a=start+j,b=a+length/2;
                float tr=wr*real[b]-wi*imag[b],ti=wr*imag[b]+wi*real[b];
                real[b]=real[a]-tr;imag[b]=imag[a]-ti;
                real[a]+=tr;imag[a]+=ti;
                float next=wr*step_real-wi*step_imag;
                wi=wr*step_imag+wi*step_real;wr=next;
            }
        }
    }
}
static void prepare_dsp(struct dsp_workspace *w)
{
    for(unsigned i=0;i<FRAME_SIZE;i++)w->window[i]=0.54f-0.46f*cosf(2.0f*PI_F*i/(FRAME_SIZE-1));
    float first=2595.0f*log10f(1.0f+80.0f/700.0f);
    float last=2595.0f*log10f(1.0f+7600.0f/700.0f);
    float hz[MEL_BANDS+2];
    for(unsigned i=0;i<MEL_BANDS+2;i++)hz[i]=700.0f*(powf(10.0f,(first+(last-first)*i/(MEL_BANDS+1))/2595.0f)-1.0f);
    for(unsigned m=0;m<MEL_BANDS;m++)for(unsigned k=0;k<=FFT_SIZE/2;k++) {
        float f=(float)k*UPLINK_KW_RATE/FFT_SIZE;
        float weight=f>=hz[m] && f<=hz[m+1]?(f-hz[m])/(hz[m+1]-hz[m]):
            f>hz[m+1] && f<=hz[m+2]?(hz[m+2]-f)/(hz[m+2]-hz[m+1]):0.0f;
        w->filters[m][k]=weight;
    }
    for(unsigned c=0;c<UPLINK_KW_COEFFS;c++)for(unsigned m=0;m<MEL_BANDS;m++)
        w->dct[c][m]=sqrtf(2.0f/MEL_BANDS)*cosf(PI_F*c*(m+0.5f)/MEL_BANDS);
}
int uplink_kw_extract(const int16_t *pcm,size_t samples,struct uplink_kw_features *out)
{
    if(out)memset(out,0,sizeof(*out));
    if(!pcm || !out || samples<3200 || samples>UPLINK_KW_MAX_SAMPLES)return -EINVAL;
    struct dsp_workspace *w=calloc(1,sizeof(*w));if(!w)return -ENOMEM;
    prepare_dsp(w);
    unsigned frames=(unsigned)((samples-FRAME_SIZE)/HOP_SIZE+1);
    float peak_energy=0.0f;
    for(unsigned i=0;i<frames;i++) {
        double sum=0.0;
        for(unsigned j=0;j<FRAME_SIZE;j++){double v=pcm[i*HOP_SIZE+j];sum+=v*v;}
        w->energy[i]=(float)(sum/FRAME_SIZE);
        if(w->energy[i]>peak_energy)peak_energy=w->energy[i];
    }
    /* Relative trimming preserves gain invariance. Keep 30ms onset/80ms tail
     * around frames above -20dB; reject genuinely silent/very weak input. */
    if(peak_energy<2500.0f){free(w);return -ENODATA;}
    unsigned first=0,last=frames-1;
    while(first<frames && w->energy[first]<peak_energy*0.01f)first++;
    while(last>first && w->energy[last]<peak_energy*0.01f)last--;
    first=first>3?first-3:0;last=last+8<frames?last+8:frames-1;
    if(last-first+1<16){free(w);return -ENODATA;}
    out->frames=last-first+1;
    float gain=1.0f/sqrtf(peak_energy);
    for(unsigned frame=first;frame<=last;frame++) {
        size_t offset=(size_t)frame*HOP_SIZE;
        for(unsigned j=0;j<FFT_SIZE;j++) {
            w->imag[j]=0.0f;
            if(j<FRAME_SIZE) {
                float v=pcm[offset+j],previous=offset+j?pcm[offset+j-1]:0.0f;
                w->real[j]=(v-0.97f*previous)*gain*w->window[j];
            } else w->real[j]=0.0f;
        }
        fft(w->real,w->imag);
        float mel[MEL_BANDS];
        for(unsigned m=0;m<MEL_BANDS;m++) {
            double sum=0.0;
            for(unsigned k=0;k<=FFT_SIZE/2;k++)
                sum+=(w->real[k]*w->real[k]+w->imag[k]*w->imag[k])*w->filters[m][k];
            mel[m]=logf(fmaxf((float)sum,1.0e-12f));
        }
        unsigned row=frame-first;
        out->values[row][0]=logf(fmaxf(w->energy[frame]/peak_energy,1.0e-6f));
        for(unsigned c=1;c<UPLINK_KW_COEFFS;c++) {
            float value=0.0f;for(unsigned m=0;m<MEL_BANDS;m++)value+=mel[m]*w->dct[c][m];
            out->values[row][c]=value/(10.0f*sqrtf((float)c));
        }
    }
    /* Cepstral mean normalization reduces constant microphone coloration;
     * never divide by per-utterance variance (that can amplify pure noise). */
    for(unsigned c=1;c<UPLINK_KW_COEFFS;c++) {
        double mean=0.0;for(unsigned i=0;i<out->frames;i++)mean+=out->values[i][c];
        mean/=out->frames;
        for(unsigned i=0;i<out->frames;i++)out->values[i][c]-=(float)mean;
    }
    free(w);return features_valid(out)?0:-ERANGE;
}
struct cell {float cost,weight;};
static float directional_distance(const struct uplink_kw_features *a,const struct uplink_kw_features *b)
{
    size_t width=(size_t)b->frames+1;
    struct cell *rows=malloc(2*width*sizeof(*rows));if(!rows)return -ENOMEM;
    struct cell *previous=rows,*current=rows+width;
    const float inf=FLT_MAX/16.0f;
    for(size_t j=0;j<width;j++)previous[j]=(struct cell){inf,0.0f};
    previous[0]=(struct cell){0.0f,0.0f};
    for(unsigned i=1;i<=a->frames;i++) {
        for(size_t j=0;j<width;j++)current[j]=(struct cell){inf,0.0f};
        for(unsigned j=1;j<=b->frames;j++) {
            float p=(float)(i-1)/(a->frames-1),q=(float)(j-1)/(b->frames-1);
            if(fabsf(p-q)>0.35f)continue;
            struct cell best=previous[j-1];
            if(previous[j].cost<best.cost)best=previous[j];
            if(current[j-1].cost<best.cost)best=current[j-1];
            if(best.cost>=inf)continue;
            float cost=0.0f;
            for(unsigned c=0;c<UPLINK_KW_COEFFS;c++) {
                float d=a->values[i-1][c]-b->values[j-1][c];cost+=d*d;
            }
            /* Wired/wireless phrases share their tail: emphasize the first
             * third rather than allowing an excellent tail to hide onset. */
            float weight=(p+q)*0.5f<0.34f?3.0f:1.0f;
            current[j]=(struct cell){best.cost+weight*cost/UPLINK_KW_COEFFS,best.weight+weight};
        }
        struct cell *swap=previous;previous=current;current=swap;
    }
    struct cell result=previous[b->frames];free(rows);
    return result.weight>0.0f && result.cost<inf?result.cost/result.weight:-ERANGE;
}
float uplink_kw_distance(const struct uplink_kw_features *a,const struct uplink_kw_features *b)
{
    if(!features_valid(a) || !features_valid(b))return -EINVAL;
    if(a->frames>2*b->frames || b->frames>2*a->frames)return -ERANGE;
    float first=directional_distance(a,b);if(first<0.0f)return first;
    float second=directional_distance(b,a);if(second<0.0f)return second;
    float length=fabsf(logf((float)a->frames/b->frames));
    return (first+second)*0.5f+0.01f*length;
}
int uplink_kw_add(struct uplink_kw_model *m,int label,const struct uplink_kw_features *f)
{
    if(!m || label<1 || label>2 || !features_valid(f))return -EINVAL;
    unsigned c=(unsigned)label-1;
    if(m->count[c]>=UPLINK_KW_PER_CLASS)return -ENOSPC;
    struct uplink_kw_features *target=&m->templates[c][m->count[c]++];
    uint32_t frames=f->frames;
    if(target!=f) {
        memset(target,0,sizeof(*target));
        memcpy(target->values,f->values,(size_t)frames*sizeof(f->values[0]));
    } else memset(target->values+frames,0,(UPLINK_KW_MAX_FRAMES-frames)*sizeof(f->values[0]));
    target->frames=frames;
    m->calibrated=m->validated_mask=0;
    return 0;
}
static float nearest_two(const float scores[3])
{
    float a=scores[0],b=scores[1];if(a>b){float swap=a;a=b;b=swap;}
    if(scores[2]<a){b=a;a=scores[2];}else if(scores[2]<b)b=scores[2];
    return (a+b)*0.5f;
}
int uplink_kw_calibrate(struct uplink_kw_model *m)
{
    if(!m)return -EINVAL;
    m->calibrated=m->validated_mask=0;
    if(m->count[0]!=3 || m->count[1]!=3)return -ENODATA;
    float distances[6][6]={{0}},within[2]={0},between[2]={FLT_MAX,FLT_MAX};
    for(unsigned i=0;i<6;i++) {
        if(!features_valid(&m->templates[i/3][i%3]))return -EINVAL;
        for(unsigned j=0;j<i;j++) {
            float d=uplink_kw_distance(&m->templates[i/3][i%3],&m->templates[j/3][j%3]);
            if(d<0.0f)return -ERANGE;
            distances[i][j]=distances[j][i]=d;
        }
    }
    for(unsigned c=0;c<2;c++)for(unsigned i=0;i<3;i++) {
        unsigned at=c*3+i;float same=0.0f,other[3];
        for(unsigned j=0;j<3;j++) {
            if(i!=j)same+=distances[at][c*3+j]*0.5f;
            other[j]=distances[at][(1-c)*3+j];
        }
        if(same>within[c])within[c]=same;
        float cross=nearest_two(other);if(cross<between[c])between[c]=cross;
    }
    for(unsigned c=0;c<2;c++) {
        float separation=fminf(between[c],between[1-c]);
        if(separation<=0.0001f || separation<=2.5f*fmaxf(within[c],within[1-c]))return -ERANGE;
        m->radius[c]=fmaxf(within[c]*1.5f,separation*0.08f);
        if(!isfinite(m->radius[c]) || m->radius[c]<=0.0f)return -ERANGE;
    }
    m->minimum_margin=0.25f;m->calibrated=1;return 0;
}
static int model_valid(const struct uplink_kw_model *m)
{
    if(!m || m->calibrated!=1 || m->count[0]!=3 || m->count[1]!=3 ||
       !isfinite(m->minimum_margin) || m->minimum_margin<0.20f || m->minimum_margin>0.75f)return 0;
    for(unsigned c=0;c<2;c++) {
        if(!isfinite(m->radius[c]) || m->radius[c]<=0.0f || m->radius[c]>1000000.0f)return 0;
        for(unsigned i=0;i<3;i++)if(!features_valid(&m->templates[c][i]))return 0;
    }
    return 1;
}
int uplink_kw_evaluate(const struct uplink_kw_model *m,const struct uplink_kw_features *f,struct uplink_kw_result *out)
{
    if(out)memset(out,0,sizeof(*out));
    if(!out || !features_valid(f))return -EINVAL;
    if(!model_valid(m))return -ENODATA;
    for(unsigned c=0;c<2;c++) {
        float scores[3];
        for(unsigned i=0;i<3;i++) {
            scores[i]=uplink_kw_distance(f,&m->templates[c][i]);
            if(scores[i]<0.0f)return 0; /* gross duration mismatch is unknown */
        }
        out->scores[c]=nearest_two(scores);
    }
    unsigned best=out->scores[1]<out->scores[0]?1:0,other=1-best;
    out->margin=(out->scores[other]-out->scores[best])/fmaxf(out->scores[other],1.0e-12f);
    if(out->scores[best]<=m->radius[best] && out->margin>=m->minimum_margin)out->label=(int)best+1;
    return 0;
}
int uplink_kw_validate(struct uplink_kw_model *m,int expected,const struct uplink_kw_features *f,struct uplink_kw_result *out)
{
    if(!m || expected<0 || expected>2)return -EINVAL;
    int rc=uplink_kw_evaluate(m,f,out);
    if(rc || out->label!=expected){m->validated_mask=0;return rc?rc:-ERANGE;}
    /* Exact training data cannot masquerade as an independent held-out test. */
    for(unsigned c=0;c<2;c++)for(unsigned i=0;i<3;i++) {
        const struct uplink_kw_features *t=&m->templates[c][i];
        if(f->frames==t->frames && !memcmp(f->values,t->values,(size_t)f->frames*sizeof(f->values[0]))) {
            m->validated_mask=0;return -EINVAL;
        }
    }
    m->validated_mask|=expected==1?1u:expected==2?2u:4u;return 0;
}
int uplink_kw_ready(const struct uplink_kw_model *m)
{return model_valid(m) && m->validated_mask==7;}
int uplink_kw_classify(const struct uplink_kw_model *m,const struct uplink_kw_features *f,struct uplink_kw_result *out)
{
    if(out)memset(out,0,sizeof(*out));
    if(!out)return -EINVAL;
    if(!uplink_kw_ready(m))return -EAGAIN;
    return uplink_kw_evaluate(m,f,out);
}

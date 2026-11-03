/* SPDX-License-Identifier: Apache-2.0 */
/* Synthetic signal/feature checks only. They do not measure speech accuracy. */
#include "uplink_keywords.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void feature(struct uplink_kw_features *f,float onset,float perturb)
{
    memset(f,0,sizeof(*f));f->frames=32;
    for(unsigned i=0;i<f->frames;i++)for(unsigned c=0;c<UPLINK_KW_COEFFS;c++)
        f->values[i][c]=(i<11?onset:0.0f)+perturb*sinf((float)(i*3+c*7));
}
int main(void)
{
    struct uplink_kw_features *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b)),*q=calloc(1,sizeof(*q));
    struct uplink_kw_model *m=calloc(1,sizeof(*m));
    int16_t *pcm=calloc(UPLINK_KW_MAX_SAMPLES,sizeof(*pcm)),*loud=calloc(16000,sizeof(*loud));
    assert(a && b && q && m && pcm && loud);
    assert(uplink_kw_extract(NULL,16000,a)==-EINVAL && !a->frames);
    assert(uplink_kw_extract(pcm,UPLINK_KW_MAX_SAMPLES+1,a)==-EINVAL);
    assert(uplink_kw_extract(pcm,16000,a)==-ENODATA && !a->frames);
    const double pi=3.14159265358979323846;
    for(unsigned i=0;i<16000;i++) {
        double envelope=i<1600 || i>13999?0.0:0.5-0.5*cos(2*pi*(i-1600)/12400.0);
        double signal=envelope*(sin(2*pi*180*i/16000.0)+0.4*sin(2*pi*740*i/16000.0)+0.2*sin(2*pi*1640*i/16000.0));
        pcm[i]=(int16_t)lrint(1000*signal);loud[i]=(int16_t)lrint(4000*signal);
    }
    assert(!uplink_kw_extract(pcm,16000,a));assert(!uplink_kw_extract(pcm,16000,b));
    assert(!memcmp(a,b,sizeof(*a)));assert(uplink_kw_distance(a,b)==0.0f);
    assert(!uplink_kw_extract(loud,16000,b));float gain_distance=uplink_kw_distance(a,b);
    assert(gain_distance>=0.0f && gain_distance<0.04f);
    assert(fabsf(gain_distance-uplink_kw_distance(b,a))<0.000001f);
    b->values[0][0]=NAN;assert(uplink_kw_distance(a,b)==-EINVAL);
    a->frames=0;assert(uplink_kw_distance(a,b)==-EINVAL);
    for(unsigned c=1;c<=2;c++)for(unsigned i=0;i<3;i++) {
        feature(q,c==1?1.0f:-1.0f,0.005f*(i+1));assert(!uplink_kw_add(m,(int)c,q));
    }
    assert(uplink_kw_add(m,1,q)==-ENOSPC);assert(!uplink_kw_calibrate(m));
    assert(!uplink_kw_ready(m));struct uplink_kw_result r;
    feature(q,1.0f,0.012f);assert(uplink_kw_classify(m,q,&r)==-EAGAIN && !r.label);
    assert(!uplink_kw_evaluate(m,q,&r) && r.label==1 && r.margin>0.25f);
    assert(!uplink_kw_validate(m,1,q,&r));
    feature(q,-1.0f,0.012f);assert(!uplink_kw_validate(m,2,q,&r));
    feature(q,0.0f,0.5f);assert(!uplink_kw_validate(m,0,q,&r));assert(uplink_kw_ready(m));
    feature(q,-1.0f,0.011f);assert(!uplink_kw_classify(m,q,&r) && r.label==2);
    feature(q,0.0f,0.0f);assert(!uplink_kw_classify(m,q,&r) && !r.label); /* ambiguous onset */
    feature(q,9.0f,0.3f);assert(!uplink_kw_classify(m,q,&r) && !r.label); /* outside radius */
    feature(q,1.0f,0.011f);assert(uplink_kw_validate(m,2,q,&r)==-ERANGE && !uplink_kw_ready(m));
    assert(uplink_kw_validate(m,1,&m->templates[0][0],&r)==-EINVAL); /* training reuse */
    feature(a,0.0f,0.0f);feature(b,0.0f,0.0f);
    for(unsigned i=0;i<10;i++)for(unsigned c=0;c<UPLINK_KW_COEFFS;c++)b->values[i][c]=1.0f;
    float onset=uplink_kw_distance(a,b);
    feature(b,0.0f,0.0f);
    for(unsigned i=22;i<32;i++)for(unsigned c=0;c<UPLINK_KW_COEFFS;c++)b->values[i][c]=1.0f;
    assert(onset>uplink_kw_distance(a,b));
    memset(m,0,sizeof(*m));feature(q,1.0f,0.0f);
    for(unsigned c=1;c<=2;c++)for(unsigned i=0;i<3;i++)assert(!uplink_kw_add(m,(int)c,q));
    assert(uplink_kw_calibrate(m)==-ERANGE && !uplink_kw_ready(m)); /* inseparable classes */
    memset(m,0,sizeof(*m));feature(&m->templates[0][0],1.0f,0.01f);
    assert(!uplink_kw_add(m,1,&m->templates[0][0]) && m->templates[0][0].frames==32);
    printf("PASS: deterministic MFCC, finite output, silence/bounds, gain distance=%g, symmetric/prefix DTW, enrollment separation, held-out readiness, ambiguity/OOD rejection\n",gain_distance);
    free(a);free(b);free(q);free(m);free(pcm);free(loud);return 0;
}

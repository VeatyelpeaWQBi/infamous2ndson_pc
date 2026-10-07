/* Mouse-to-Orbis motion. Acceleration is in g, angular velocity in rad/s.
 * The physical pose determines gravity; the reset reference only changes the
 * reported orientation. No integration drift or external sensor library. */
#ifndef BB_PAD_MOTION_H
#define BB_PAD_MOTION_H
#include <math.h>
#include <string.h>
#include <stdint.h>
typedef struct {
    float target[2], angle[2], velocity[2], acceleration[2];
    float physical[4], reference[4];
    uint64_t time;
    int active;
} PadMotion;

static float motion_clamp(float v,float limit) { return fmaxf(-limit,fminf(limit,v)); }
static void motion_multiply(const float a[4],const float b[4],float q[4]) {
    const float v[4]={a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    memcpy(q,v,sizeof(v));
}
static void motion_relative(const float reference[4],const float physical[4],float q[4]) {
    const float inverse[4]={-reference[0],-reference[1],-reference[2],reference[3]};
    motion_multiply(inverse,physical,q);
}
static void motion_pose(PadMotion *m) {
    const float yaw[4]={0,sinf(m->angle[0]/2),0,cosf(m->angle[0]/2)};
    const float pitch[4]={sinf(m->angle[1]/2),0,0,cosf(m->angle[1]/2)};
    const float upright[4]={0,0,0.7071067811865475f,0.7071067811865475f};
    float q[4]; motion_multiply(yaw,pitch,q); motion_multiply(q,upright,m->physical);
}
static void motion_center(PadMotion *m) {
    memset(m->target,0,sizeof(m->target)); memset(m->angle,0,sizeof(m->angle));
    memset(m->velocity,0,sizeof(m->velocity)); memset(m->acceleration,0,sizeof(m->acceleration));
    motion_pose(m);
}
static void motion_reset_orientation(PadMotion *m) { memcpy(m->reference,m->physical,sizeof(m->reference)); }
static void motion_step(PadMotion *m,uint64_t now,int active,int center,float dx,float dy,
                        float sensitivity,float q[4],float a[3],float w[3]) {
    memset(a,0,3*sizeof(float)); memset(w,0,3*sizeof(float));
    memset(q,0,4*sizeof(float)); q[3]=1;
    if (!active) {
        memset(m,0,sizeof(*m)); m->reference[3]=m->physical[3]=1;
        return;
    }
    const int fresh=!m->active || !m->time || now<=m->time || now-m->time>250000;
    float dt=fresh ? 1.f/60.f : (float)(now-m->time)/1000000.f;
    dt=fmaxf(dt,0.0001f);
    if (fresh) { motion_center(m); memset(m->reference,0,sizeof(m->reference)); m->reference[3]=1; }
    if (center) motion_center(m);
    m->active=1; m->time=now;
    float previous[4]; memcpy(previous,m->physical,sizeof(previous));
    if (!isfinite(dx)) dx=0;
    if (!isfinite(dy)) dy=0;
    if (!isfinite(sensitivity) || sensitivity<=0) sensitivity=0.003f;
    const float delta[2]={motion_clamp(dx,4096),motion_clamp(dy,4096)};
    const float blend=1.f-expf(-dt/0.025f);
    for (int i=0;i<2;++i) {
        m->target[i]=motion_clamp(m->target[i]-delta[i]*sensitivity,1.15f);
        m->angle[i]+=(m->target[i]-m->angle[i])*blend;
        const float velocity=delta[i]/dt;
        const float force=motion_clamp((velocity-m->velocity[i])*0.0001f/dt,4.f);
        m->acceleration[i]+=(force-m->acceleration[i])*(1.f-expf(-dt/0.035f));
        m->velocity[i]=velocity;
    }
    motion_pose(m);
    motion_relative(m->reference,m->physical,q);
    float rotation[4]; motion_relative(previous,m->physical,rotation);
    for (int i=0;i<3;++i) w[i]=motion_clamp(2.f*rotation[i]/dt,20.f);
    const float x=m->physical[0],y=m->physical[1],z=m->physical[2],s=m->physical[3];
    // Mouse translation is in the screen plane (+right, +down). Rotate it
    // into controller axes, so up/down shaking remains vertical while upright.
    const float ax=m->acceleration[0],ay=-m->acceleration[1];
    a[0]=2*(x*y+z*s)+(1-2*(y*y+z*z))*ax+2*(x*y+z*s)*ay;
    a[1]=1-2*(x*x+z*z)+2*(x*y-z*s)*ax+(1-2*(x*x+z*z))*ay;
    a[2]=2*(y*z-x*s)+2*(x*z+y*s)*ax+2*(y*z-x*s)*ay;
}
#endif

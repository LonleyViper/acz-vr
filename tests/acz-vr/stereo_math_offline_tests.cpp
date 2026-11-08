// Pure numerical regressions. Fixtures are synthetic, not native calibration.
#include "stereo_renderer.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace acz;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
namespace {
constexpr double pi=3.14159265358979323846;
void close(double a,double b) { CHECK(std::isfinite(a) && std::abs(a-b)<1e-9); }
void equal(const Vec4& a,const Vec4& b) { for(int i=0;i<4;++i) close(a[i],b[i]); }
void equal(const Mat4& a,const Mat4& b) { for(int i=0;i<16;++i) close(a[i],b[i]); }
template<class F> void rejects(F fn) {
    bool caught=false; try { fn(); } catch(const std::exception&) { caught=true; }
    CHECK(caught);
}
FrameState fixture() {
    FrameState f{}; f.shouldRender=true;
    f.eyes[0]={{{.03,0,-.1},{0,0,0,1}},
        {std::atan(-1.),std::atan(2.),std::atan(1.5),std::atan(-.5)},true};
    f.eyes[1]={{{-.03,0,-.1},{0,0,0,1}},
        {std::atan(-2.),std::atan(1.),std::atan(.5),std::atan(-1.5)},true};
    return f;
}
void composition() {
    StereoRenderer renderer;
    const auto aircraft=poseToWorld({{10,2,3},{0,std::sin(pi/4),0,std::cos(pi/4)}});
    const auto seat=translation(0,1,-2);
    renderer.setAircraftPose(aircraft,seat);
    auto frame=fixture();
    renderer.computeEyeMatrices(frame,identity());
    const auto& eye=renderer.eye(0);
    equal(transformPoint(eye.T_W_E,{0,0,0}),Vec4{7.9,3,2.97,1});
    equal(transformVec4(eye.T_W_E,{0,0,-1,0}),Vec4{-1,0,0,0});
    equal(transformVec4(eye.V_E_W,{7.9,3,2.97,1}),Vec4{0,0,0,1});

    // Noncommuting recenter, tilted seat, aircraft rotation/translation,
    // and calibration. Oracle transforms a point through each domain separately.
    const auto recenter=computeRecenter({1,2,3},pi/3);
    const auto tiltedSeat=poseToWorld({{0,1,-2},{std::sin(pi/8),0,0,std::cos(pi/8)}});
    const auto calibration=poseToWorld({{3,-1,2},{0,0,std::sin(pi/6),std::cos(pi/6)}});
    renderer.setAircraftPose(aircraft,tiltedSeat);
    renderer.setDrawCPTCalibration(calibration);
    renderer.computeEyeMatrices(frame,recenter);
    Vec4 point{.4,-.2,-.8,1};
    auto expected=transformVec4(poseToWorld(frame.eyes[0].pose),point);
    expected=transformVec4(recenter,expected);
    expected=transformVec4(tiltedSeat,expected);
    expected=transformVec4(aircraft,expected);
    equal(transformVec4(renderer.eye(0).T_W_E,point),expected);

    Vec4 native{2,1,-4,1};
    auto world=transformVec4(calibration,native);
    auto eyePoint=transformVec4(renderer.eye(0).V_E_W,world);
    auto clip=transformVec4(renderer.eye(0).P,eyePoint);
    equal(transformVec4(renderer.eye(0).VP_N,native),clip);
    auto wrong=transformVec4(multiply(multiply(aircraft,tiltedSeat),
        multiply(recenter,poseToWorld(frame.eyes[0].pose))),point);
    CHECK(std::abs(wrong[0]-expected[0])>0.1);

    CapturedDraw draw{}; draw.localToWorld=toFloat32(translation(1,2,3));
    const auto substituted=renderer.substituteDraw(0,draw);
    auto throughLocal=transformVec4(fromFloat32(draw.localToWorld),native);
    equal(transformVec4(substituted.localToClip,native),
          transformVec4(renderer.eye(0).VP_N,throughLocal));
}
void asymmetricProjection() {
    StereoRenderer renderer; auto frame=fixture();
    for(auto& eye:frame.eyes) eye.pose={{0,0,0},{0,0,0,1}};
    StereoConfig cfg; cfg.nearZ=.1; cfg.farZ=100; cfg.flipY=false;
    renderer.computeEyeMatrices(frame,identity(),cfg);
    for(int i=0;i<2;++i) {
        const auto& p=renderer.eye(i).P;
        auto center=transformPoint(p,{0,0,-1});
        close(center[0]/center[3],i==0 ? -1./3 : 1./3);
        close(center[1]/center[3],i==0 ? -.5 : .5);
        close(center[2]/center[3],100./111);
        for(double d:{.1,2.,100.}) {
            auto l=transformPoint(p,{d*std::tan(frame.eyes[i].fov.angleLeft),0,-d});
            auto r=transformPoint(p,{d*std::tan(frame.eyes[i].fov.angleRight),0,-d});
            auto b=transformPoint(p,{0,d*std::tan(frame.eyes[i].fov.angleDown),-d});
            auto t=transformPoint(p,{0,d*std::tan(frame.eyes[i].fov.angleUp),-d});
            close(l[0]/l[3],-1); close(r[0]/r[3],1);
            close(b[1]/b[3],-1); close(t[1]/t[3],1);
        }
        auto n=transformPoint(p,{0,0,-.1}), f=transformPoint(p,{0,0,-100});
        close(n[2]/n[3],0); close(f[2]/f[3],1);
    }
}
void recenter() {
    Pose head{{1,2,3},{0,std::sin(pi/4),0,std::cos(pi/4)}};
    double yaw=0; CHECK(extractHeadingYaw(head.orientation,yaw)); close(yaw,pi/2);
    auto r=computeRecenter(head.position,yaw);
    close(r[12],3); close(r[13],-2); close(r[14],-1);
    equal(multiply(poseToWorld(head),r),identity());
    // Head-relative eye offset survives recenter, without a second head motion.
    auto eyePose=multiply(translation(.032,0,0),poseToWorld(head));
    equal(transformPoint(multiply(eyePose,r),{0,0,0}),Vec4{.032,0,0,1});
    // q_yaw * q_pitch in column convention: heading yaw removed, tilt retained.
    double y=pi/3, p=pi/6;
    Quat tilted{std::cos(y/2)*std::sin(p/2), std::sin(y/2)*std::cos(p/2),
                -std::sin(y/2)*std::sin(p/2), std::cos(y/2)*std::cos(p/2)};
    CHECK(extractHeadingYaw(tilted,yaw)); close(yaw,y);
    auto recentered=multiply(poseToWorld({head.position,tilted}),computeRecenter(head.position,yaw));
    equal(transformVec4(recentered,{0,0,-1,0}),Vec4{0,.5,-std::sqrt(3.)/2,0});
    CHECK(!extractHeadingYaw({std::sin(pi/4),0,0,std::cos(pi/4)},yaw));
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    const auto inf=std::numeric_limits<double>::infinity();
    for(double invalid:{nan,inf,-inf}) {
        rejects([&]{computeRecenter({invalid,0,0},0);});
        rejects([&]{computeRecenter({0,invalid,0},0);});
        rejects([&]{computeRecenter({0,0,invalid},0);});
        rejects([&]{computeRecenter({0,0,0},invalid);});
        rejects([&]{extractHeadingYaw({invalid,0,0,1},yaw);});
        rejects([&]{extractHeadingYaw({0,0,0,1},yaw,invalid);});
    }
    rejects([&]{extractHeadingYaw({0,0,0,0},yaw);});
    rejects([&]{extractHeadingYaw({0,0,0,1},yaw,-1);});
}
void validity() {
    StereoRenderer r; auto frame=fixture(); r.computeEyeMatrices(frame,identity());
    CHECK(r.eye(0).valid && r.eye(1).valid);
    frame.eyes[0].valid=false; r.computeEyeMatrices(frame,identity());
    CHECK(!r.eye(0).valid && r.eye(1).valid);
    CapturedDraw draw{};
    rejects([&]{r.substituteDraw(0,draw);});
    rejects([&]{r.substituteDraw(-1,draw);});
    rejects([&]{r.substituteDraw(2,draw);});
    frame.eyes[0].valid=true;
    frame.eyes[1].fov.angleLeft=frame.eyes[1].fov.angleRight;
    rejects([&]{r.computeEyeMatrices(frame,identity());});
    CHECK(!r.eye(0).valid && !r.eye(1).valid); // atomic publish on numerical failure
    frame.shouldRender=false; r.computeEyeMatrices(frame,identity());
    CHECK(!r.eye(0).valid && !r.eye(1).valid);
}
void finiteMatrices() {
    const auto frame=fixture();
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const double inf=std::numeric_limits<double>::infinity();
    // Every entry, including off-diagonal NaNs missed by inverse's scale
    // reduction, must be rejected in all four externally supplied matrices.
    for(double invalid:{nan,inf,-inf}) for(int entry=0;entry<16;++entry) {
        Mat4 bad=identity(); bad[entry]=invalid;
        rejects([&]{inverse(bad);});
        for(int input=0;input<4;++input) {
            StereoRenderer r;
            r.computeEyeMatrices(frame,identity()); // clear previously valid eyes
            CHECK(r.eye(0).valid && r.eye(1).valid);
            if(input==0) r.setDrawCPTCalibration(bad);
            if(input==1) r.setAircraftPose(bad,identity());
            if(input==2) r.setAircraftPose(identity(),bad);
            rejects([&]{r.computeEyeMatrices(frame,input==3 ? bad : identity());});
            CHECK(!r.eye(0).valid && !r.eye(1).valid);
        }
    }

    // Left eye succeeds; right-eye VP overflows from entirely finite operands.
    // A failure late in the loop must not publish the already-computed left eye.
    auto overflowFrame=fixture();
    for(auto& eye:overflowFrame.eyes) eye.pose={{0,0,0},{0,0,0,1}};
    overflowFrame.eyes[0].fov={std::atan(-2.),std::atan(2.),std::atan(2.),std::atan(-2.)};
    overflowFrame.eyes[1].fov={std::atan(-.5),std::atan(.5),std::atan(.5),std::atan(-.5)};
    Mat4 calibration=identity(); calibration[0]=std::numeric_limits<double>::max();
    StereoRenderer r;
    r.setDrawCPTCalibration(calibration);
    overflowFrame.eyes[1].valid=false;
    r.computeEyeMatrices(overflowFrame,identity());
    CHECK(r.eye(0).valid && std::isfinite(r.eye(0).VP_N[0]));
    overflowFrame.eyes[1].valid=true;
    rejects([&]{r.computeEyeMatrices(overflowFrame,identity());});
    CHECK(!r.eye(0).valid && !r.eye(1).valid);

    // Finite pose-chain operands and finite FOV/depth inputs can overflow too.
    r.setDrawCPTCalibration(identity());
    Mat4 huge=identity(); huge[0]=std::numeric_limits<double>::max();
    Mat4 twice=identity(); twice[0]=2;
    r.setAircraftPose(twice,huge);
    rejects([&]{r.computeEyeMatrices(frame,identity());});
    CHECK(!r.eye(0).valid && !r.eye(1).valid);
    r.setAircraftPose(identity(),identity());
    auto tinyFov=frame;
    tinyFov.eyes[1].fov.angleLeft=0;
    tinyFov.eyes[1].fov.angleRight=std::numeric_limits<double>::denorm_min();
    rejects([&]{r.computeEyeMatrices(tinyFov,identity());});
    CHECK(!r.eye(0).valid && !r.eye(1).valid);
    StereoConfig overflowDepth; overflowDepth.nearZ=1e200; overflowDepth.farZ=2e200;
    rejects([&]{r.computeEyeMatrices(frame,identity(),overflowDepth);});
    CHECK(!r.eye(0).valid && !r.eye(1).valid);

    // Infinite far plane is intentional; its resulting matrices remain finite.
    StereoConfig cfg; cfg.farZ=inf;
    r.computeEyeMatrices(frame,identity(),cfg);
    for(int i=0;i<2;++i) {
        CHECK(r.eye(i).valid);
        requireFiniteMatrix(r.eye(i).T_W_E); requireFiniteMatrix(r.eye(i).V_E_W);
        requireFiniteMatrix(r.eye(i).P); requireFiniteMatrix(r.eye(i).VP_N);
        close(r.eye(i).P[10],-1); close(r.eye(i).P[14],-cfg.nearZ);
        auto nearPoint=transformPoint(r.eye(i).P,{0,0,-cfg.nearZ});
        close(nearPoint[2]/nearPoint[3],0);
    }
}
}
int main() {
    try {composition(); asymmetricProjection(); recenter(); validity(); finiteMatrices();
         std::cout<<"Stereo row composition, asymmetric FOV, recenter, validity and finite-matrix regressions passed\n";}
    catch(const std::exception& e) {std::cerr<<e.what()<<"\n"; return 1;}
}

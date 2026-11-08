#include "fake_l3_fault_matrix.h"
#include <cstdio>
#include <cstdlib>
using namespace acz::fake_l3_fault;
namespace { unsigned checks=0;
#define CHECK(x) do { ++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);} }while(0)
void refused(Fault fault) { Session s(1);CHECK(s.arm(11,22));s.fault(fault);CHECK(s.state()==State::Quarantined);CHECK(s.refusal()==fault);CHECK(!s.output_published());CHECK(!s.native_proof_available());CHECK(!s.shutdown()); }
void faults() { refused(Fault::Reentry);refused(Fault::AmbiguousSubmission);refused(Fault::Timeout);refused(Fault::DeviceLost);refused(Fault::InFlightTeardown); }
void lifecycle() { Session s(1);CHECK(s.arm(11,22));CHECK(!s.arm(11,22));CHECK(s.refusal()==Fault::Reentry);CHECK(!s.output_published()); Session invalid_cap(0);CHECK(!invalid_cap.arm(1,2));CHECK(invalid_cap.refusal()==Fault::CapExceeded); Session cap(1);CHECK(cap.arm(1,2));CHECK(cap.complete(1,2));CHECK(!cap.arm(3,4));CHECK(cap.refusal()==Fault::CapExceeded); Session mismatch(1);CHECK(mismatch.arm(1,2));CHECK(!mismatch.complete(1,3));CHECK(mismatch.refusal()==Fault::AmbiguousSubmission); Session clean(2);CHECK(clean.arm(1,2));CHECK(clean.complete(1,2));CHECK(clean.state()==State::Retired);CHECK(clean.shutdown());CHECK(!clean.arm(3,4));CHECK(clean.refusal()==Fault::Reentry);CHECK(!clean.output_published());CHECK(!clean.native_proof_available()); }
}
int main(){faults();lifecycle();std::printf("%u fake L3 fault/shutdown checks passed; no native/runtime claim\n",checks);}

// SDK method-order check only; does not instantiate any COM or GPU object.
#define CINTERFACE
#include <d3d12.h>
#include <cstddef>
#include "gpu_copy_observation_slots.h"
using namespace acz::gpu_copy;
#define SLOT(T, M, N) static_assert(offsetof(T##Vtbl,M)==sizeof(void*)*(N),"SDK slot: " #T "::" #M)
#define BASE(T) SLOT(T,Close,9); SLOT(T,Reset,10); SLOT(T,ClearState,11); \
    SLOT(T,ExecuteBundle,27); SLOT(T,ExecuteIndirect,59); \
    SLOT(T,BeginQuery,slots::begin_query); SLOT(T,EndQuery,slots::end_query); \
    SLOT(T,ResolveQueryData,slots::resolve_query); SLOT(T,SetPredication,slots::predication)
#define PROTECTED(T) SLOT(T,SetProtectedResourceSession,slots::protected_session)
#define PASS(T) SLOT(T,BeginRenderPass,slots::begin_pass); SLOT(T,EndRenderPass,slots::end_pass)
BASE(ID3D12GraphicsCommandList);
BASE(ID3D12GraphicsCommandList1);
BASE(ID3D12GraphicsCommandList2);
BASE(ID3D12GraphicsCommandList3); PROTECTED(ID3D12GraphicsCommandList3);
BASE(ID3D12GraphicsCommandList4); PROTECTED(ID3D12GraphicsCommandList4); PASS(ID3D12GraphicsCommandList4);
BASE(ID3D12GraphicsCommandList5); PROTECTED(ID3D12GraphicsCommandList5); PASS(ID3D12GraphicsCommandList5);
BASE(ID3D12GraphicsCommandList6); PROTECTED(ID3D12GraphicsCommandList6); PASS(ID3D12GraphicsCommandList6);
BASE(ID3D12GraphicsCommandList7); PROTECTED(ID3D12GraphicsCommandList7); PASS(ID3D12GraphicsCommandList7);
BASE(ID3D12GraphicsCommandList8); PROTECTED(ID3D12GraphicsCommandList8); PASS(ID3D12GraphicsCommandList8);
BASE(ID3D12GraphicsCommandList9); PROTECTED(ID3D12GraphicsCommandList9); PASS(ID3D12GraphicsCommandList9);
BASE(ID3D12GraphicsCommandList10); PROTECTED(ID3D12GraphicsCommandList10); PASS(ID3D12GraphicsCommandList10);
SLOT(ID3D12CommandQueue,ExecuteCommandLists,10);
int main() {return 0;}

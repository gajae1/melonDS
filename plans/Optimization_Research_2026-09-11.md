# melonDS 1.1.32 최적화 연구 후보 (2026-09-11)

이 문서는 연구 후보 정리이며 구현 결정이나 성능 이득 증거가 아니다. 이 연구 라운드에서 최적화 벤치마크는 수행되지 않았다. 1.1.32의 통과한 릴리스 테스트는 별도로 구현된 정확성 변경의 검증이며, 아래 제안 최적화의 효과 증명이 아니다. 본문의 모든 예상 효과는 측정 전 가설이다.

## 우선순위 결론

기본 순서는 다음과 같다. 각 단계의 측정 결과가 다음 단계의 착수 여부를 결정한다.

1. 횡단 계측 확대 — 게스트 CPU 실행, JIT 컴파일/조회/탈출/무효화, GPU 업로드/리드백/대기, 오디오 백로그, 호스트 네트워크 큐/대기를 분리한 카운터와 기존 paired median/tail 프로토콜.
2. 생성 코드 실험 — PGO train/holdout, LTO/ThinLTO 비교, ISA 디스패치 실측.
3. JIT 뜨거운 비용 — 디스패치 빈도, 코드 페이지 보호 fault/패치, 사이클 누적 비용을 계측한 뒤에만 변경 검토.
4. GPU 중복 전송 — 업로드 바이트/비용 측정이 스트리밍·재사용·배치 변경에 선행.
5. 네트워크 관찰 후 안정 자동조절 — 계측이 적응 정책보다 먼저다.

큰 병렬화(ARM9/ARM7 직접 스레드화)와 스케줄러 변경(이벤트 순서, 반복 양자화)은 게스트 가시 타이밍을 바꿀 수 있는 고위험 실험이다. 병렬 실행에서도 기존 순서를 보존할 수 있는지가 먼저다. 현재 기본값은 검증 전까지 유지한다.

## 현재 기반

이미 있는 기능: JIT(ARM9/ARM7), 소프트웨어 3D 렌더러 워커 스레드, 프런트엔드 오디오 소비자 분리, 런타임 디스패치 SIMD(AVX2/AVX512 픽셀 커널). 멀티코어가 전혀 없는 구조가 아니다. 코어 CPU의 직접 병렬화와 이미 분리된 렌더·오디오 소비를 구분한다.

현재 구성: C23/C++26, Windows GCC 16.2 Release -O3. 현재 빌드 캐시는 AVX512를 켜고 두 LTO 옵션을 모두 끈 상태이며, 소스 기본값은 AVX512 OFF다. 구성된 바이너리와 소스 기본값을 구분해야 한다. GCC15+에서 LTO를 강제 OFF하는 guard가 [cmake/DefaultBuildFlags.cmake:11](../cmake/DefaultBuildFlags.cmake#L11)에 있고 주석은 ICE를 이유로 든다. 이번 라운드에서 그 ICE는 재현되지 않았고 guard 삭제 근거도 없다. PGO train/use 워크플로는 구현되어 있지 않다.

## 후보 표

| # | 후보(현상) | 코드 시작점 | 다음 bounded 실험 | 성공/기각·위험 | 계획 ID |
|---|---|---|---|---|---|
| 1 | 횡단 계측 확대 — 도메인별 비용이 분리되어 있지 않아 후보 우선순위가 추측에 의존 | [src/NDS.cpp](../src/NDS.cpp) RunFrame/RunSystem 기준 실행 경로 | 동일 소스·입력·설정에서 cold/warm, 인터프리터/JIT/fastmem, 렌더 백엔드를 구분한 paired median/tail 측정. 레지스터/flags/사이클/프레임/프레임 중간 읽기 보존 확인 포함 | 성공: 후보 2~12의 순위가 데이터로 결정. 계측 비용이 결과를 왜곡하거나 판단에 쓰이지 않는 카운터는 줄이거나 제거한다. 마이크로벤치만으로 게임 수용 판단 금지 | BV-10 확장(기존) |
| 2 | PGO + LTO/ThinLTO — 두 워크플로 모두 미측정 상태. LTO는 GCC15+ guard로 강제 OFF | [cmake/DefaultBuildFlags.cmake:11](../cmake/DefaultBuildFlags.cmake#L11), [CMakeLists.txt](../CMakeLists.txt) LTO 옵션부 | 별도 워크로드로 train하고 별도 holdout에서 median/tail 평가. 컴파일러/프로파일 신원 기록. guard를 유지한 채 지원 컴파일러에서 ON/OFF(thin/full) 비교와 리마크 확인. [GCC PGO 문서](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html)와 [Clang 문서](https://clang.llvm.org/docs/UsersManual.html)의 수집/사용 절차를 참조 | 성공: 사용한 컴파일러 조합의 안정성·정확성 유지와 holdout 개선. 기각: 오차 범위 또는 빌드/시작 시간 회귀. 위험: 프로파일 대표성 부족, guard 삭제 금지, ICE 주석은 미재현 상태 | BV-09 + BV-08(기존) |
| 3 | ISA 디스패치·AVX512 — 소스 기본 OFF, 현재 캐시는 ON. 구성 차이만으로 이득을 단정할 근거 없음 | [cmake/HostFeatures.cmake](../cmake/HostFeatures.cmake), [src/PixelConvert.cpp:36](../src/PixelConvert.cpp#L36) target 커널 | AVX2/AVX512를 실측 비교하고 다운클록 가능성을 함께 관찰. 소스 기본과 구성된 빌드를 문서에서 구분 | 성공: 실측 이득 + 정상 대비 손실 없음. 기각: 다운클록 등 손실 우세. 위험: 플랫폼별 편차, 이 빌드가 이미 ON이라 "활성화"가 아니라 "측정"이 과제 | BV-12(기존), GR-09 연관 |
| 4 | 컴파일러 리마크·데이터레이아웃 — 인라인/벡터화 누락 존재 여부 미확인. NonStupidBitfield 정렬은 가설이지 알려진 결함이 아님 | [src/GPU2D_Soft.cpp](../src/GPU2D_Soft.cpp) 템플릿 특수화, [src/NonStupidBitfield.h](../src/NonStupidBitfield.h) | -fopt-info 계열 리마크로 특수화 커널의 인라인/벡터화 상태 확인. 기존 비트필드 벤치는 진단용으로만 사용. 누락이 확인되면 핫 커널 1개를 골라 소규모 검토 | 성공: 실제 리마크 근거 확보. 기각: 누락 없으면 코드 변경 없음으로 종료. 위험: 리마크 문구는 컴파일러 버전 의존 | 빌드검증 워크스트림 내 신규 |
| 5 | JIT 뜨거운 비용 계측 — 기존 프로파일링은 iJIT/VTune 이름 통보뿐이고 실행 카운트·fault 원인 분류가 없음 | [src/ARMJIT.cpp](../src/ARMJIT.cpp) InvalidateByAddr, [src/ARMJIT_x64/ARMJIT_LoadStore.cpp](../src/ARMJIT_x64/ARMJIT_LoadStore.cpp) RewriteMemAccess | 블록 실행/디스패치 도달/컴파일·조회/무효화/패치·보호 토글 카운터(릴리스 기본 OFF)를 수집하고 계측/무계측 비용을 비교. fault는 원인별로 분류. 페이지 크기는 호스트별 [src/ARMJIT_Memory.cpp](../src/ARMJIT_Memory.cpp) PageSize 기준으로 기술 | 성공: 후보 6·7의 착수 근거 확보. 기각: 부팅 초기에만 발생하고 정상 프레임에서 0에 수렴하면 현 구조 유지. 오버헤드가 작다고 미리 가정하지 않음 | CJ-18 + BV-10 통합(기존) |
| 6 | JIT 직접 블록 체이닝 — 블록마다 dispatcher 왕복이 있으나 도달 빈도는 미측정(기존 분기-follow가 연속 구간을 이미 흡수) | [src/ARM.cpp](../src/ARM.cpp) 블록 디스패치, [src/ARMJIT_x64/ARMJIT_Linkage.S](../src/ARMJIT_x64/ARMJIT_Linkage.S) ARM_Dispatch/ARM_Ret | 먼저 계측(후보 5)으로 dispatcher 도달 비용을 재고, 유의하면 ARM7 한정 프로토타입으로 패치된 점프와 무효화 시 원복을 검증. [QEMU의 block chaining/SMC 설명](https://qemu.readthedocs.io/en/master/devel/tcg.html)은 이 연결 수명의 참고 자료 | 성공: 정확성 회귀 통과 + 체인 경계별 이벤트/IRQ/StopExecution/HALT/MPU/SMC 무효화/사이클 예산 보존 + flags·사이클 동일. 기각: 도달 빈도가 낮거나 개선이 오차 수준. 위험: 무조건 분기만으로는 안전 조건이 불충분, W^X·OS별 검증 필요 | CJ 워크스트림 내 신규(CJ-15/18 연관) |
| 7 | x64 사이클 누적 전용 레지스터 — x64는 조건·비정규 경로에서 메모리 RMW, A64는 전용 레지스터. 호스트 할당 폭도 7-8개 vs 15개로 다름 | [src/ARMJIT_x64/ARMJIT_Compiler.cpp](../src/ARMJIT_x64/ARMJIT_Compiler.cpp) Comp_AddCycles_C/CI, 대조 [src/ARMJIT_A64/ARMJIT_Compiler.cpp](../src/ARMJIT_A64/ARMJIT_Compiler.cpp) | 블록 덤프로 RMW 감소를 확인하고 조건 명령 비중이 다른 두 시나리오로 A/B 비교. 사이클 레지스터와 spill 비용을 함께 평가 | 성공: 고밀도 시나리오 개선 + 저밀도 무손실. 기각: spill·헬퍼 저장 증가로 전반 저하. 위험: 모든 헬퍼 콜과 탈출 지점에서 정확한 사이클 계정 유지 필수 — 사이클 오차는 게임 기능 파손 가능. A64는 비교 참조이지 정확성 기준이 아님 | CJ 워크스트림 내 신규 |
| 8 | GPU 캡처 readback — 캡처 행마다 즉시 동기 glReadPixels 왕복. 비동기 회수 구조 없음 | [src/GPU_OpenGL.cpp:915](../src/GPU_OpenGL.cpp#L915) SyncCaptureLines | API 추적으로 프레임당 호출 수·시점 기록 후, 겹쳐 실행할 작업이 있는 경우에만 PBO 링+fence를 검토하며 첫 게스트 소비 전에 회수를 완료. ([glReadPixels](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glReadPixels.xhtml), [glFenceSync](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glFenceSync.xhtml)) | 성공: 최종 프레임 바이트·게스트 CPU readback 값 동일 + 대기 감소. 기각: 데이터 가시성 위반 또는 mid-capture 보존 실패. 위험: 가시성 완료는 첫 게스트 CPU/DMA/JIT 소비자보다 먼저여야 하고 fence 단독으로 대기가 사라지지 않음. 다중 적시 readback은 정상. 프레임당 1회를 요구하지 않음 | 렌더러 워크스트림 내 신규(GR-05/06/14 연관) |
| 9 | Compute 업로드·프레임 스킵 — 매 논-아이덴티컬 프레임 버퍼 재업로드, 텍스처만 갱신돼도 지오메트리 재업로드 | [src/GPU3D_Compute.cpp:1004](../src/GPU3D_Compute.cpp#L1004) 업로드 경로 | 업로드 바이트/호출 비용 카운트가 선행. 이후 persistent map/orphaning/fence 가드 또는 지오메트리 불변 시 부분 스킵을 검토 | 성공: 출력 동일 + 전송 감소. 기각: 출력 차이 즉시 보류. 위험: FlushRequest 단독은 재사용 키로 불충분(텍스처 갱신이 레이어·variant·span 구성을 바꿀 수 있음). GL4.3 환경에 미지원 GL4.4 persistent storage를 필수로 요구하지 않고 기존 경로 유지. indirect producer/한도 항목은 GR-07 정확성 작업이 먼저 | GR-14 측정 선행 + 렌더러 워크스트림 내 신규 |
| 10 | 텍스처 업로드 배치 — 캐시 미스마다 개별 bind+upload. 배치 전송 없음 | [src/GPU3D_TexcacheOpenGL.cpp](../src/GPU3D_TexcacheOpenGL.cpp) 업로드 경로 | 프레임당 신규 텍스처 수·업로드 호출 수 카운트 후 배치 전송을 검토. PBO staging만으로 서로 다른 텍스처 목적지의 호출이 하나로 합쳐지지는 않는다. 갱신 가시 순서 보존 확인 | 성공: 최종 프레임 바이트 동일 + 텍스처 지연 표시 없음. 기각: 갱신 순서가 어긋나면 보류. 위험: 스캔라인 중 업로드는 게스트 타이밍과 얽힘, 구형 드라이버 PBO 품질 편차 | 렌더러 워크스트림 내 신규 |
| 11 | 네트워크 관찰 후 bounded 자동조절 — RecvTimeout은 호스트 밀리초 대기 고정 기본값이고 ENet 통계 중 RTT만 복사 중 | [src/net/MPInterface.h](../src/net/MPInterface.h) RecvTimeout, [src/net/LAN.cpp](../src/net/LAN.cpp) 관측 지점 | 먼저 RTT/지터/큐 시계열 계측(읽기 전용). 분포가 확보되면 상한·복귀 기본값·취소/메모리/지연 예산에서 경계를 도출해 수동 고정값·오프라인 재생·자동안을 비교. ([ENet 헤더](https://raw.githubusercontent.com/lsalzman/enet/master/include/enet/enet.h), [RFC 8085](https://www.rfc-editor.org/info/rfc8085/)) | 성공: 같은 정상/열악 환경에서 기본값 대비 연결성과 취소 응답성을 유지하고 정한 지연 예산을 만족하면서 꼬리 지연 개선. 기각: 수렴하는 조절 규칙이 도출되지 않으면 고정값 유지. 위험: 사용자 고정 설정·v1 wire·구버전 피어 보존 필수. ENet packetLoss를 unsequenced MP 유실률로 해석 금지. RTT 단독으로 로컬 에뮬레이션 정체를 분류하지 않음. [RFC 6298](https://www.rfc-editor.org/info/rfc6298/)은 TCP RTO이지 MP 수신 대기 정책이 아님. 연결 타임아웃/LocalMP 그룹/Netplay 정리는 기존 NP-02/04/09 범위로 귀속 | 연결성 워크스트림 내 신규(NP-02 진단 보조) |
| 12 | 병렬화·스케줄러 고위험 후보 — RunSystem 이벤트 순서, 반복 양자화 상수, SPU mix 분리, ARM9/ARM7 직접 스레드화. 현재는 ID 순 실행·고정 상수·단일 스레드 코어 | [src/NDS.cpp](../src/NDS.cpp) RunSystem/NextTarget, [src/SPU.cpp](../src/SPU.cpp) Mix | 계측 선행: due 이벤트 다중 case 로그, 반복 상수별 trace 비교, Mix 비용 분해, ARM7 busy 비율과 공유 상태 경합 목록 확정. ([QEMU MTTCG 문서](https://www.qemu.org/docs/master/devel/multi-thread-tcg.html) — 메모리/MMIO/IRQ/코드 무효화 동기화 책임의 개념 참조) | 성공: 독립 오라클과 실기 대조를 통과할 때만 채택 검토. 기각: 측정된 이득이 없거나 경합 비용이 더 크면 유지. 짧은 관측에서 문제가 없었다고 전체 안전성을 확정하지 않음. HIGH 위험: 게스트 가시 타이밍 변경이므로 검증 전까지 현재 기본값 유지. 독립 오라클 없는 재정렬 금지. DSi dynamic_cast은 DMA/NDMA 분기에만 존재 — 생성 코드와 빈도 확인 전에 비효율로 단정 금지. SPU Mix는 mic/I2S·게스트 MMIO를 함께 전진시키므로 임의 mix-ahead 도입 금지. 직접 스레드화는 장기 조사로만 유지 | CJ-08/CJ-20 연관, AD-04/AD-05 연관, 워크스트림 내 신규 |

## C/C++/어셈블리 전환에 대하여

C++에서 C로의 일괄 전환 또는 어셈블리 전환이 이득을 가져온다는 근거는 없다. 이 저장소에는 이미 템플릿 특수화와 명시 SIMD 커널이 있다. [src/PixelConvert.cpp](../src/PixelConvert.cpp)에는 명시 target 속성 커널이 있고 [src/GPU2D_Soft.cpp](../src/GPU2D_Soft.cpp)에는 백엔드별 템플릿 특수화가 있다. [GCC 인라인 문서](https://gcc.gnu.org/onlinedocs/gcc/Inline.html)는 세 가지 인라인 의미론(GNU C89/C99 이상/C++)을 구분하므로 C99과 C++의 인라인 의미론이 동일하다고 가정할 수 없다. 절차는 생성 코드를 먼저 검사하고 리마크로 실제 누락을 확인한 뒤, 선정된 핫 커널 한 곳에만 선택적으로 intrinsics나 어셈블리를 검토하는 순서다. 저장소 전체 언어 전환은 이 연구에서 근거가 없다.

## 정확성 보존과 명시적 정확성 변화의 구분

두 종류의 실험은 채택 기준이 다르다.

- 정확성 보존 최적화: 출력·레지스터·flags·사이클·게스트 가시 순서를 보존하는 변경(전송 중복 제거, 컴파일 플래그, JIT 실행 비용 등). 기존 정확성 회귀·동일 출력 oracle과 실제 성능 개선이 채택 조건이다.
- 명시적 정확성 변화 선택: 스케줄러 이벤트 순서, 반복 양자화, 스레드화 실험에서 게스트 가시 타이밍이 실제로 달라지는 경우. 구체적인 편차를 이름 붙이고 실기(실제 하드웨어) 비교로 효과를 검증하기 전에는 기본 도입하지 않는다. 이번 연구로 어떤 정확성 완화 모드도 활성화되지 않았다.

## 남은 게이트

이번 조사로 새 최적화를 구현하지 않았고 성능 향상 수치도 주장하지 않는다. 네이티브 ARM64 호스트, Android, Linux, 다른 GPU, DSi, 실제 멀티플레이와 물리 기기 검증은 열린 게이트로 유지된다. 구버전 .sav/설정/LAN v1 계약을 유지한다. 기존 14.x 상태를 읽는 것과 원본이 새 14.2를 읽는 것은 별개이며, 14.2 미지원 구버전의 역방향 한계는 [1.1.32 호환성 기록](releases/1.1.32.md)을 따른다. macOS 빌드와 GitHub Actions는 현재 개발 목표에서 제외다. 크로스 컴파일이나 소스 검토만으로 네이티브 수용을 대체할 수 없다.

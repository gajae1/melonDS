# 현재 개발 상태 - 1.1.126

기준 소스: `4a6b2e7177914869b2af6bd542de408711c4c305`.
제품 버전은 **1.1.126**이다. 이 문서는 2026-09-15의 상태 색인이며 새 기능 출시 선언이 아니다.
이전 [버전별 계획](Release_Plan.md)의 1.1.03~1.1.44 번호는 원래 책임 배정이며, 현재 제품 버전 상한이 아니다.

## 최근 변경과 현재 남은 범위

| 버전 | 저장소에 반영된 범위 | 아직 완료로 해석하면 안 되는 것 |
|---|---|---|
| 1.1.122 | Vulkan 3D 내부 해상도 1x/2x/3x, scale별 shader와 자원 크기 | 확대 3D를 그대로 보여 주는 최종 두 화면 합성 |
| 1.1.123 | 오디오 장치 teardown 대기 상한 | 모든 물리 장치의 지연·종료 문제 해결 |
| 1.1.124 | Windows atomic save replace의 제한적 재시도 | 자연 발생 접근 거부의 원인 제거·전원 손실 내구성 |
| 1.1.125 | 경로 변경 시 이미 완료한 저장 데이터를 새 대상에 재기록하지 않기 | 모든 ROM·GBA·firmware 교체 시나리오 검증 |
| 1.1.126 | ROMSmoke가 확대 GL front buffer의 좌상단만 읽던 오류 수정 | Software/Classic/Compute/Vulkan의 실기 정확성 일치 |

위 변경의 기존 검증 기록은 [Validation.md](Validation.md)와 [Task_Catalog.md](Task_Catalog.md)에 남긴다. 이번 문서 검토가 과거 검사를 전부 재실행했다는 뜻은 아니다.

## 다음 구현: D-004 / GR-15

사용자 결정 D-004는 Vulkan도 확대 front buffer를 표시하도록 하는 것이다. **현재 제품에는 아직 미구현**이다.
현재 `GPU3D_Vulkan.cpp`는 확대 3D 결과에서 native pixel origin을 골라 256x192로 변환한다. `GPU_Vulkan.cpp`는 native `SoftRenderer`의 2D·표시를 사용한다.

구현 시 다음 경계를 각각 유지한다.

1. **게스트 경로:** 1x의 기존 색·알파·절삭 및 CPU/DMA가 읽는 native capture를 보존한다.
2. **표시 경로:** 실제 확대 3D subpixel을 2D layer/window/OBJ/밝기 규칙에 맞춰 합성한다. native 완성 화면을 단순 확대하는 것은 구현 완료가 아니다.
3. **수명:** scale 변경·상태 저장/복원·중단·renderer 교체에서 유효한 이전 frame을 보존하고, 새 자원 준비 실패는 명시적으로 반환한다.
4. **소비자:** Native QImage 복사, GL RAM texture upload, paused-frame 보존, ROMSmoke가 각각 실제 width/height/stride를 사용하도록 한다.
5. **수락:** 확대 표시의 향상 효과와 native 캡처의 동등성을 별도 검사한다. Classic 또는 여러 renderer의 일치를 실기 oracle로 사용하지 않는다.

확인한 코드 시작점:
`src/GPU.h`, `src/GPU_Soft.cpp/.h`, `src/GPU2D_Soft.cpp/.h`,
`src/GPU3D_Vulkan.cpp/.h`, `src/GPU_Vulkan.cpp/.h`,
`src/frontend/qt_sdl/Screen.cpp/.h`, `src/frontend/qt_sdl/EmuInstance.cpp`,
`tests/VulkanRenderer.cpp`, `tests/ROMSmoke.cpp`.

## 이번 실행에서 확보한 근거

- Windows x64에서 설치된 CMake 4.4.3 / GCC 16.2.0 / Vulkan shader toolchain으로 기존 GPU test target을 빌드했다. LTO는 OFF였다.
- 기존 Vulkan 검사 6개가 6/6 통과했다. 이는 synthetic GPU/presentation 범위이며, 전체 앱·전체 CTest·게임·실기 검증이 아니다.
- 기본 native view를 반환하는 임시 display-view 인터페이스와 확대 extent 검사를 추가한 경우, 2x/3x 두 검사가 모두 `scaled Vulkan display extent is still native`로 실패했다. 이는 미구현 표시 계약의 반례이며 실기 그래픽 결함을 재현한 것이 아니다.
- 확대 합성 구현은 파일 쓰기 도구의 보안 판정에서 중단됐다. 미완성 diff와 red-test 입력은 로컬 작업 증거에 별도 보존했고, 프로덕션 코드와 CTest 등록은 기준 소스로 복원했다.
- 복원한 기준 소스로 GPU test target을 재빌드한 뒤 같은 Vulkan 6개 검사가 6/6 통과했다(6.26초, 실패/skip 0). `src/`, `tests/`, 루트 CMake에는 변경이 남지 않았다.
- 1.1.127 구현·성능 향상·실기 오류 해결을 선언하거나 미완성 코드를 master에 병합하지 않았다. GitHub Actions와 macOS 빌드는 실행하지 않았다.

## 독립적으로 남은 조사

화면 불일치는 동일 입력·native 배율에서 3D 출력, 2D 합성, guest capture, 최종 표시의 첫 차이를 분리한다.
Vulkan/OpenGL 최적화는 기존 `MELONDS_RENDER_DIAGNOSTICS` 계측을 먼저 확인하고, upload/readback/CPU 합성/present 비용을 구분한다.
DSi 실기 실행·NAND/BIOS 덤프는 사용자 준비 이후 별도 진행하며, 이 문서 작업에서는 개인 runtime 파일을 사용하지 않았다.

# 현재 개발 상태 — 1.1.134

2026-09-16. 기준은 1.1.133 `7e771592`다. 현재 변경과 실제 검증은 [1.1.134 기록](releases/1.1.134.md)을 따른다.
메인 작업/빌드 경로는 `F:/melonDS`와 `F:/melonDS/build/windows-dev`다. 과거 릴리스의 실행 이력은 보존한다.

| 영역 | 현재 구현 | 남은 경계 |
|---|---|---|
| GR-11/16 표시 수명 | paint의 잠금 안에서 framebuffer 재조회, 사라진 source의 기존 이미지 보존, 실패 fallback 교체 잠금 | 사용자 crash와의 동일성, 장기/다중 창/물리 device loss의 모든 경우 |
| Vulkan 확대 표시 | 1~16배 직접 3D와 LCDC VRAM 직접 표시용 고해상도 사본 유지 | 캡처의 bitmap BG/OBJ·3D texture 재사용은 native |
| native 캡처 | CPU/DMA용 RGB555 VRAM 기록 유지 | 실기 기대값과 모든 캡처·alias 경계 검증 |
| 캐시/전송 | RAM-only cache·비우기·32MiB 소프트 제한, clear 이미지 묶음·cached readback | GPU 2D·직접 GPU 표시·프레임 전체 업로드 묶음 |
| 저장 | 1.1.132의 Windows 교체 재시도·직렬화1회·이전 파일 보존 유지 | 외부 잠금 원인·전원 손실 내구성 |
| 오디오 | 기존 PCM·보간·필터·종료 소유권 정책 유지 | 실제 장기 청취·장치 탈착·물리 지연 |

## 이번에 닫은 반례

별도 보존돼 있던 `paint consumed retired framebuffer at 2x`는 수정 전 재현 후 수정본에서 통과했다.
NativeFrameLifetime을 기본 회귀에 등록했다. 여러 배율의 전체 subpixel 데이터, source 부재·null·잘못된 크기와 paused image 보존을 검사한다.
기존 ComputeFailure는 실제 shader compile/link 실패의 fallback에서 잠금 유지와 반납을 확인하도록 보강했다.
이전 테스트 수치에 이 반례를 소급 포함하지 않는다. 최종 빌드/전체 테스트 결과는 릴리스 기록에 기재한다.

## 다음 단계

1. 캡처의 bitmap BG/OBJ 경로부터 고해상도 표시 사본을 연결한다. native VRAM, CPU/DMA 쓰기·bank alias·상태 복원 무효화 기준을 먼저 확정한다.
2. 캡처를 3D texture로 재사용하는 경우의 표시 향상을 별도로 설계한다. native guest 결과와 enhanced texture를 섞어 정확성 통과로 처리하지 않는다.
3. 실기와 다른 native 화면은 같은 입력의 3D→2D→capture→표시 중 최초 차이를 찾아 독립 관측으로 판정한다.
4. 실제 게임의 업로드·readback·CPU 합성·표시 비용을 각각 측정한다. Vulkan이라는 이유만으로 전체 FPS 우위를 가정하지 않는다.

이번 버전은 캡처 BG/OBJ/texture 개선이나 전체 Vulkan 최적화의 완료가 아니다. Classic도 실기 전체의 정답으로 고정하지 않는다.
원125개 과제 ID와 역사적 책임 버전은 유지한다. Actions·다른 작업트리·개인 ROM/BIOS/NAND/저장 파일·드라이버 전역 캐시는 변경하지 않는다.

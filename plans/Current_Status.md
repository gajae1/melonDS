# 현재 개발 상태 — 1.1.127

2026-09-16. 구현 기준은 1.1.126 `4a6b2e71`, 작업 시작점은 문서/fixture 수정이 포함된 `55dba795`다.
이번 변경은 [1.1.127 구현·검증 기록](releases/1.1.127.md)으로 묶는다. 전체 로드맵 완료나 실기 정확성 인증은 아니다.

## 이번에 연결한 기능

| 영역 | 현재 구현 | 남은 범위 |
|---|---|---|
| D-004 / GR-15 | Vulkan 2x/3x의 실제 3D 샘플을 CPU 2D 레이어와 합성한 확대 표시 버퍼, Native/GL 표시 소비자 연결 | GPU 2D/capture, GPU image 직접 표시, 4x 이상, 다른 GPU/OS 및 장기 게임 |
| native 캡처 | 기존 256×192 RAM/capture 경로 유지. 표시 합성 때문에 guest 상태나 캡처를 재실행하지 않음 | 실기와의 픽셀·시점 판정은 독립 실기 입력 필요 |
| 표시 수명 | 크기 변경 전 자원 준비, 이전 프레임 유지, 고해상도 일시정지 이미지, GL 업로드 크기 갱신 | 물리 surface/device loss·메모리 고갈·복수 모니터 장기 사용 |
| AD-04 | pending open이 destructor 대기 뒤 완료돼도 원래 제어 스레드가 결과/장치를 해제 | 영구 정지한 드라이버 취소, 장기 청취·물리 지연·장치 탈착 |
| 검증 도구 | ROMSmoke의 RAM/GL 확대 화면을 같은 box resolve로 비교, 오래된 renderer fixture/설정창 기대값 갱신 | 게임별 화면/PCM 회귀는 이번 실행 범위 밖 |

3D 배율 향상과 DS 실기 정확성은 다른 축이다. Classic/Software/Compute/Vulkan 중 하나를 전역 정답으로 사용하지 않는다.
2D 스프라이트/배경 자체의 세부 해상도는 native이며, 확대된 3D를 native 완성 화면의 단순 확대와 구분한다.

## 확인한 근거

Windows x64, 설치된 CMake 4.4.3 / GCC 16.2.0, C23/C++26, LTO OFF로 전체 앱과 관련 검사 타깃을 빌드했다.
관련 CTest 110개가 실패/skip 0으로 통과했다. 이후 추가한 GL 확대 할당 실패 검사는 별도 RED/GREEN 로그로 보존한다.
Vulkan 전체 화면·텍스처/캡처·상태 복원·배율 전환, CPU 합성, 실제 GL 업로드 및 오디오 상태 검사가 포함된다.
실제 Windows 기본 출력의 SDL/WASAPI 무음 start/stop/resume/close 4조건도 통과했다. 청감이나 물리 지연을 측정한 것은 아니다.
전체 CTest·게임/BIOS/NAND·DSi 실기·다른 OS를 이번 결과에 포함하지 않는다. Actions와 macOS 빌드는 실행하지 않았다.

## 다음 작업의 우선순위

1. native 1x의 실제 화면 차이를 3D 결과 → 2D 합성 → guest capture → 최종 표시로 분리하고, 공개 시험/실기 관측값으로 판정한다.
2. 새 확대 경로를 실제 게임 입력과 Native/GL/Vulkan 표시에서 대조한다. RGB6/alpha 절삭·가림·window·capture를 바꾸지 않는다.
3. 기존 render diagnostics로 CPU 합성, texture upload, readback, present 비용을 분리하고 반복 측정으로 이득이 확인된 최적화만 채택한다.
4. 오디오의 최소위상/16채널 가변 timer 비용, 장기 공급 부족 및 물리 장치 상실을 별도 측정한다. 이번 종료 수정으로 음질·지연 개선을 주장하지 않는다.

과거 1.1.03~1.1.44 번호는 [원래 책임 배정](Release_Plan.md)이다. 125개 과제 ID와 기존 검증 이력을 유지한다.
이전 중단 작업과 미완성 패치는 과거 상태이며, 지금의 구현 여부는 이 문서와 해당 릴리스 기록을 따른다.

# 오디오 점검 상태 — 1.1.126

2026-09-15. 기준 제품 소스 `4a6b2e71`, 작업 브랜치의 문서 기준 `48966ff9`.
이 점검은 전체 오디오의 청감·물리 지연 또는 모든 장치의 정상 동작을 인증하지 않는다.

## 실행한 검사

Windows x64의 CMake 4.4.3 / GCC 16.2.0, 기존 로컬 SDK와 CTest를 사용했다.
오디오 설정 UI, 마이크 경계, callback, 장치 복구/종료, time-stretch, 상태 복원,
Minimum-phase, SIMD 선택, core clock와 one-shot 상태의 기존 검사 **38/38 통과**, 실패/skip 0(13.97초).
UI/마이크/device fixture의 대역·SDL dummy 사용과 실제 SPU/상태 검사를 구분한다.
실제 게임 청취, 물리 endpoint 탈착, 출력 지연과 장기 부하 검사는 이번에 수행하지 않았다.

검사 준비 중 `StateLoadMessages`가 `RendererUsesOpenGL` 선언 누락으로 빌드되지 않았다.
`RendererSelection.h`를 포함하고 복제한 Software renderer 상수를 제거하여 컴파일을 복구했다.
이는 테스트 fixture 수정이며 PCM 생성·오디오 장치 구현 변경이 아니다.

## 새로 재현한 미해결 결함 — AD-04, pending open의 종료 소유권

`AudioOutput::~AudioOutput()`의 Close가 outstanding open을 기다리다 시간 초과되고,
`Owner::~Owner()`까지 진입한 뒤 native open이 완료되면, 결과 future에 남은 Impl이
생성한 제어 스레드가 아니라 호출 스레드에서 소멸한다. 현재 Owner의 종료 분기는
future/retiring/job을 정리하지 않고 exited만 게시한다.
기존 FrontendAudio의 SDL dummy/생성 스레드 기록에 결정적인 timeout 동기화를 추가하여
`close count=1, wrong owner=1, exit observed=1`, exit 1로 재현했다.
수정안은 제어 스레드에서 미시작 job을 취소하고 결과/retiring을 정리한 뒤 exited를 게시하는 것이다.

원본 Owner 제어 코드를 별도 Linux C++ 검사에 그대로 추출했다. 장치는 대역이다.
원본은 3개 수명 시나리오 중 2개 통과, 수정 후보는 GCC 및 Clang ASan/UBSan에서 3개 통과했다.
이는 Windows SDL/WASAPI 통합 검증이 아니다. 사용자 PC의 프로덕션 수정 요청은 도구 단계에서 차단되어 **수정안은 아직 미반영**이다.
실패 재현용 테스트 diff와 원본 실행 로그는 로컬 작업 증거에 보존하고, 일반 CTest 소스는 원상복원했다.
기존 38개 검사를 통과했다는 사실로 새로 재현한 이 결함을 닫지 않는다.

## 다음 수락 조건

1. 수정안과 보존한 FrontendAudio 회귀를 함께 적용해 Windows에서 실패→통과를 확인한다.
2. pending open의 성공/실패, 미시작 요청 취소, Close timeout 후 재개방과 파괴의 소유권을 확인한다.
3. 장치 관련 영향 검사와 실제 SDL/WASAPI 재개·종료를 확인한 뒤 프로덕션 수정으로 승격한다.

기존 Stop의 callback 대기와 Owner의 마지막 join은 여전히 무제한일 수 있다.
이 수정 후보는 잘못된 스레드에서의 자원 해제를 다루며, 영구 정지한 native API를 취소하지 않는다.
스레드를 detach하거나 살아 있는 callback의 데이터를 해제해 대기만 제거하는 방식은 사용하지 않는다.
Vulkan 확대 표시는 별도 D-004 작업이며 이번 점검에서 구현·성능·실기 정확도가 개선된 것은 아니다.

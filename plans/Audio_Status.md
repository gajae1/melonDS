# 오디오 점검 상태 — 1.1.127

2026-09-16. 기준 제품 1.1.126 `4a6b2e71`, 후속 문서/fixture `55dba795`에서 이어진 수정이다.
이번 수정은 출력 장치 수명에 한정한다. SPU PCM 계산·Minimum-phase 계수·보간 선택·볼륨·필터 기본값은 바꾸지 않았다.

## 해결한 종료 소유권 결함 — AD-04

1.1.126에서 native open이 `Close`와 `Owner`의 제한 대기보다 늦게 완료되면,
future에 남은 장치가 생성한 제어 스레드가 아니라 호출 스레드에서 해제될 수 있었다.
이전 점검은 반례와 Linux 대역 후보까지만 보존했다. 이번에는 Windows의 실제 product 코드에 수정했다.

제어 스레드의 종료 분기가 미시작 job을 취소하고 미회수 result·retiring 장치를 정리한 뒤 exited를 게시한다.
네이티브 해제 동안 handoff mutex를 잡지 않으며, live thread/callback을 detach하거나 메모리를 먼저 해제하지 않는다.

보존한 반례를 이번 Windows 실행에서 다시 적용했다.
수정 전 `close count=1, wrong owner=1, exit observed=1`로 실패했고,
수정 후 같은 pending-open 검사와 기존 callback/loss/reopen/async-owner 검사가 통과했다.
관련 오디오·설정·상태·Minimum-phase 검사는 1.1.127의 관련 CTest 110개 묶음에 포함된다.
UI/SDL dummy/합성 PCM 검사와 실제 OS 출력 검증은 서로 다른 근거다.

## 실제 Windows 출력 경로 점검

제품 `melonds-audio-output`/`melonds-wasapi` 라이브러리에 로컬 전용 검사기를 연결했다.
기본 endpoint에 무음만 전달하며 Open→Start→Stop→Resume→Close를 실행했다.

| 경로 | 요청 frame | 확인한 rate / period | 결과 |
|---|---:|---|---|
| SDL의 WASAPI 드라이버 | 128 | 48000 Hz / 128 | 통과 |
| SDL의 WASAPI 드라이버 | 512 | 48000 Hz / 512 | 통과 |
| WASAPIShared | 128 | 48000 Hz / 480 | 통과 |
| WASAPIShared | 512 | 48000 Hz / 480 | 통과 |

Stop 뒤 source callback 증가가 멈추고 Resume 뒤 다시 증가하는 것과 Close 성공을 확인했다.
이 period는 물리 speaker 지연이 아니다. endpoint는 OS의 현재 기본값이며 실제 청취·탈착·방송 루프백 검사는 아니다.

## 남은 범위

- `PauseCallbacks`와 destructor의 최종 join은 native 작업 완료를 기다린다. timeout은 영구 정지한 API를 취소하지 않는다.
- 실제 게임 잡음, 장기 청취, underrun·물리 왕복 지연, 장치 분리/재연결 및 다른 OS는 별도 수락 대상이다.
- 최소위상 보간의 고부하 비용과 음질 수락은 종료 소유권 수정과 별개다.

기존 38개 통과 기록과 그 뒤 발견된 반례는 과거 근거로 유지한다. 이번 결과로 전체 오디오가 무결하다고 선언하지 않는다.
재현·실패·수정 후 로그와 로컬 무음 검사기는 `build/evidence/resume-20260916/`에 보존한다.

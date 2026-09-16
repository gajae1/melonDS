# 오디오 점검 상태 — 1.1.130

최신 재검증은 아래 1.1.130 항목을 따른다. 먼저 남긴 1.1.127 구현 이력은 당시 기준으로 보존한다.

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


## 1.1.129 후속 확인 — 2026-09-16

이번 제품 변경은 렌더링 버퍼 수명·전송 비용이며 오디오 PCM·최소위상 계수·필터·기본 설정은 바꾸지 않았다.
전체 Windows CTest960개 중 오디오/오디오 상태 복원32개도 모두 통과했다. 실패/skip은0이다.
실제 Windows 기본 endpoint에서 현재 AudioOutput 라이브러리로 무음 Open→Start→Stop→Resume→Close를 재실행했다.
SDL requested128/512:48000Hz, actual period128/512; WASAPIShared requested128/512:48000Hz, actual period480/480. 네 조건 모두 통과했다.
정지 동안 client callback 증가가 멈추고 재개 뒤 증가하는 것을 확인했다. 이 값은 음질·장치 탈착·장기 부하·물리 지연의 판정이 아니다.
영구 정지한 드라이버와 callback drain/최종join의 무제한 대기 가능성은 계속 열려 있다.
근거: [1.1.129 구현·검증](releases/1.1.129.md), 로컬 `build/evidence/costs-1.1.129/native-audio-test.log`.

## 1.1.130 재검증 — 2026-09-16

오디오 제품 소스·SPU PCM·보간/필터 계수는 이번 버전에서 변경하지 않았다.
현재 구성의 전체 CTest 962개(실패0·skip0)에 오디오 callback/장치 복구·설정/상태 복원과 SPU 회귀가 포함된다.
최종 AudioOutput 라이브러리에 다시 링크한 무음 실행기도 실제 Windows 기본 endpoint에서 실행했다.
SDL(WASAPI 드라이버)과 WASAPIShared 각각 요청128/512의 Open→Start→Stop→Resume→Close 4조건이 통과했다.
48000Hz, SDL period128/512·WASAPIShared period480/480을 관측했다. 정지 중 source callback 정지와 재개를 확인했다.
이는 짧은 무음 제어 시험이며 실제 게임 음질·장기 부족·물리 지연·핫플러그 또는 모든 장치의 수락이 아니다.
로그는 `build/evidence/continue-1.1.130/native-audio-test.log`에 있다.

# 현재 개발 상태 — 1.1.131

2026-09-16. 기준은 1.1.130 `012bc4e2`다. [1.1.131 구현·측정·검증](releases/1.1.131.md)과 [캐시 정책](Caching.md)을 따른다.
이전 1.1.03~1.1.44는 책임 배정이며 모든 과제의 완료 목록이나 현재 버전 상한이 아니다.

| 영역 | 현재 구현 | 남은 경계 |
|---|---|---|
| GR-14/15 캐시 | RAM-only Vulkan cache, UI 비우기와 worker 요청/응답, 32MiB export 데이터 기준 자동 비우기 | 드라이버 내부 총 RAM/VRAM 하드 상한·전역 디스크 캐시 관리는 아님 |
| GR-15 clear 전송 | 색/깊이 이미지의 staging 준비·submit/fence를 한 번으로 묶음, 할당 실패 전 상태 보존 | 모든 texture 업로드의 프레임 단위 묶음·비동기화 |
| GR-14 GL | 1.1.129의 실제 batch scratch 제한 유지, 비활성 파일 binary cache 코드 제거 | 실제 게임별 CPU/GPU 병목·이종 드라이버 |
| GR-14/15 readback | cached/coherent 메모리 선택과 fallback, 재사용 CPU 버퍼 유지 | 통합 GPU·다른 메모리 구조에서의 실측 |
| D-004 표시 | 기존1~16배 확대 3D 표시와 native capture 분리 유지 | GPU 2D/capture·직접 GPU 이미지 표시 |
| AD-04 오디오 | 이전 종료 소유권·PCM/보간/필터 정책 유지 | 장기 청취·물리 지연·hotplug·정지 드라이버 취소 |
| BV-07 | 현재 UI와 캐시 설명, 기존 과제 ID 보존 | 모든 플랫폼의 빌드·드라이버 수락은 별도 |

캐시 비우기는 현재 pipeline·텍스처·프레임을 버리지 않는다. 비우기 기능 때문에 guest 실행이나 캡처를 다시 수행하지 않는다.
Classic/Software/Compute/Vulkan 중 어느 하나도 실기 전체의 정답으로 고정하지 않는다.
배율 향상과 native 실기 정확성은 별도 축이다. 기존 실제 게임 화면 불일치를 이번 전송·캐시 변경으로 해결했다고 하지 않는다.

## 이후 작업

1. native 화면 차이는 같은 입력·같은 시점의 3D 결과→2D 합성→guest capture→표시로 분리하고 독립 관측으로 기대값을 확보한다.
2. 현재 완료한 clear 두 장 묶음과 구분해, texture upload 제출을 프레임 단위로 묶을 때의 저장 수명·실패 rollback·guest 가시성을 설계하고 측정한다.
3. GPU 2D·직접 표시 전에 scanline register/window/FIFO/capture의 시간 계약과 플랫폼 표시 복구를 고정한다.
4. 8→9배 tile 전환과 pipeline 생성·readback·CPU 합성 비용을 실게임 holdout에서 비교한다. 합성 루틴 수치를 전체 FPS로 확대하지 않는다.
5. 오디오는 기존 회귀 통과와 장기 청취·장치 탈착·물리 지연을 구분한다. 캐시 작업에 맞춰 계수나 PCM을 임의 변경하지 않는다.

원125개 과제 ID와 과거 기록은 Task_Catalog/Release_Plan/Validation/Audio_Status에 보존한다.
Actions·macOS 빌드는 사용하지 않는다. 개인 ROM·BIOS·NAND·저장 파일이나 드라이버 전역 캐시를 삭제하지 않는다.

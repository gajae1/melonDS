# 현재 개발 상태 — 1.1.139

2026-09-18. 메인 작업/빌드 경로는 `F:/melonDS`, `build/windows-dev`다.
현재 변경의 범위·실제 검증은 [1.1.139 기록](releases/1.1.139.md)을 따른다.
1.1.136 Windows 패키지 provenance는 2026-09-18 자체 manifest 기준으로 재확인했다. 패키지 바이너리 행동 검증은 이 세션에서 재실행하지 않았다.

| 영역 | 현재 구현 | 남은 경계 |
|---|---|---|
| 표시 수명 | 1.1.134의 paint 버퍼 재조회와 실패 fallback 잠금 유지 | 사용자 crash의 직접 대조, 장기/다중 창/실제 device loss |
| Vulkan 확대 | 1~16배 직접 3D·LCDC 표시, bitmap BG2/BG3 및 일반·반전·affine direct-color bitmap OBJ의 캡처 디테일, Vulkan 렌더러·표시 GPU 어댑터 선택과 활성 GPU 상태 표시 | 모자이크 OBJ 향상, 3D texture 재사용, 색인 bitmap 향상; 변환 OBJ 검증은 양 engine 1·3·5배율; 실기 다중 GPU·핫플러그 미검증 |
| GL compute 이식성 | texture uniform 호출을 texture 래스터라이저 프로그램으로 한정해 strict 드라이버(AMD Windows GL 포함)에서 설정 적용 중단 수정 | 다른 GL 구현·드라이버 버전 전수 검증은 별도 |
| 비트맵 합성 | 양 engine의 layer 후보·창·우선순위·효과·affine, native VRAM 기록 분리 | 전체 GPU 2D/capture가 아니며 CPU 고배율 비용이 큼 |
| native 보존 | CPU/DMA용 캡처 기록, provenance 무효화·중복 mapping fallback | 실기 oracle와 모든 게임 경계의 수락은 별도 |
| 캐시/전송 | RAM-only cache·비우기·32MiB 소프트 제한, clear 묶음 전송·cached readback | 전체 texture 제출 묶음·GPU 직접 표시 |
| 오디오/저장 | 기존 PCM·보간·종료 소유권·저장 파일 교체 재시도 유지 | 장기 청취·탈착·물리 지연·전원 손실 내구성 |

기존에 중단된 비트맵 변경 7개 파일을 먼저 보존한 뒤 검토·확장했다.
1:1 bitmap 좌표의 반복 modulo/division을 줄였다. 합성 시험의 개선을 게임 FPS나 실기 정확성으로 확대하지 않는다.
1.1.136은 같은 배율의 정수 affine에서 픽셀별 캡처 조회를 subx 루프 밖으로 옮겼다. 전체 행을 하나의 span으로 조회하는 확장은 아직 없다.

## 다음 단계

1. 비트맵 합성 최적화는 추출 CPU 하네스의 양 engine·일반 affine 출력 바이트 대조를 통과했다. 실제 GPU 캡처 수명 대조와 engine B·일반 affine 성능은 남아 있다. 추가 span 묶음은 측정 후 결정한다.
2. 일반·반전·affine bitmap OBJ의 캡처 재사용을 구현했다. 모자이크 향상과 고배율 성능은 다음 범위다. 모자이크 행·무효 provenance·중복 mapping·배율 불일치는 native fallback을 유지한다.
3. 3D texture 재사용의 표시용 샘플과 native guest 결과를 분리한다. 정확성 판정을 위해 향상 경로를 정답으로 삼지 않는다.
4. GPU 2D·직접 표시와 texture 업로드 묶음을 작은 단계로 진행하고 scanline/FIFO/capture 가시성·복구 수명을 보존한다.
5. 실제 게임의 3D→2D→capture→표시 중 첫 차이와 CPU/GPU 비용을 각각 측정한다. Classic도 실기 전체의 정답으로 고정하지 않는다.

기존 버전별 runtime/source 패키지 도구를 재사용한다. 새 버전 디렉터리는 구분하며 이전 사용자 빌드·ROM·저장 파일은 삭제하지 않는다.
원125개 과제 ID와 역사적 책임 버전은 유지한다. Actions·다른 작업트리·드라이버 전역 캐시는 변경하지 않는다.

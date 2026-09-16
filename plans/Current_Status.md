# 현재 개발 상태 — 1.1.129

2026-09-16. 기준 소스는 1.1.128 `739ec2eb`다. 현재 결과와 실제 검증은 [1.1.129 기록](releases/1.1.129.md)을 따른다.
과거 1.1.03~1.1.44는 책임 배정이며 현재 버전 상한이나 전체 완료 목록이 아니다.

| 영역 | 현재 상태 | 남은 경계 |
|---|---|---|
| GR-14 OpenGL Compute | 실제 batch work에 맞춘 임시 버퍼, 축소된 할당과 일치하는 배율 한도 검사 | 전체 게임 비용·다른 GPU에서의 한도 및 메모리 고갈 |
| GR-15 Vulkan 전송 | 동기 완료 뒤 staging 재사용, 실패한 성장의 기존 버퍼 보존 | 업로드 묶음 제출·비동기 복사·GPU 직접 표시 |
| GR-14/15 readback | 재사용 CPU 캐시 메모리에 memcpy 후 borrowed view로 즉시 소비 | GPU HOST_CACHED 우선 선택·통합 GPU별 메모리 정책은 미적용 |
| D-004 확대 표시 | 기존 1~16배·확대 3D와 native 캡처 분리를 유지 | 실기 정확성과는 다른 축; GPU 2D/capture는 미구현 |
| AD-04 오디오 | 1.1.127 종료 소유권 수정 유지, PCM·필터·보간 계수 미변경 | 장기 청취·물리 지연·장치 탈착·영구 정지 driver 호출 |
| BV-04 검증 연결 | 전체 로컬 빌드에서 발견한 GL loader fixture의 renderer 선언·Native panel 경계를 보완 | 물리 GPU/OS별 수락과 전체 로드맵 완료는 별개 |

OpenGL split-batch scratch의 과다 할당은 이번에 수정했다. 이전 문서의 미적용 상태는 당시 기록이다.
직접 GPU mapping을 읽는 후보는 실제 전체 색 변환 비용이 악화돼 제거했다. 빠른 일부 픽셀 검사만으로 채택하지 않는다.
정확성 비교는 허용 오차를 넓히지 않는다. Classic/Software/Compute/Vulkan 중 하나를 실기 정답으로 고정하지 않는다.

## 다음 작업과 필요한 증거

1. **Native 화면 불일치:** 동일 입력의 3D 출력→2D 합성→guest 캡처→표시 중 최초 차이를 분리한다. 실기 관측이나 독립적인 공개 시험이 있어야 정확성 판정을 닫는다.
2. **Vulkan 전송 묶음:** 현재 fence와 guest 소비 시점을 보존한 채 texture upload 개수/대기 시간을 측정하고, staging 재사용과 별도로 제출 묶음을 설계한다.
3. **GPU 2D·직접 표시:** scanline별 레지스터·window·FIFO·capture 가시성과 화면 수명부터 시험한다. 현재 CPU 합성 제거가 완료됐다고 하지 않는다.
4. **메모리/배율 전환:** HOST_CACHED 우선 선택과 coherent fallback, pipeline cache, 8→9배 tile 변경 비용을 독립 측정한다. 이 중 메모리 선택 후보는 이번 도구 차단으로 미적용이다.
5. **오디오:** 현재 회귀와 실제 기본 endpoint 동작을 장기 게임 청취·물리 지연과 구분한다. 살아 있는 callback을 detach하지 않는다.
6. **보조 정리:** 오래된 .ui 도움말, TOML literal 공백, Teakra의 CMake 진단 변수 변경은 도구 차단으로 미적용이며 warning-free를 선언하지 않는다.

원 과제 ID와 과거 기록은 [Task_Catalog](Task_Catalog.md), [Release_Plan](Release_Plan.md), [Validation](Validation.md), [Audio_Status](Audio_Status.md)에 유지한다.
Actions·macOS 빌드는 사용하지 않는다. 개인 ROM·BIOS·NAND·저장 파일은 이번 작업 입력으로 사용하지 않았다.

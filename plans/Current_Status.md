# 현재 개발 상태 — 1.1.128

2026-09-16. 기준 제품은 1.1.127 `c80adcd1`이다. 현재 결과는 [1.1.128 구현·검증 기록](releases/1.1.128.md)을 따른다.
과거 1.1.03~1.1.44 번호는 책임 배정이며 현재 제품 버전 상한이나 전체 완료 목록이 아니다.

## 구현과 검증 범위

| 영역 | 현재 상태 | 구분할 한계 |
|---|---|---|
| D-004 / GR-15 | Vulkan 1~16배 내부 렌더링·확대 표시, 512개 내장 셰이더, 배율별 타일/dispatch | GPU 2D/capture·GPU image 직접 표시와 전체 성능 최적화는 미완료 |
| GR-14/15 자원 | GPU 한도로 batch work와 span/scratch 용량을 제한. 기존1~3배 주요 버퍼 요청량 감소 | 실제 VRAM 총량·FPS 향상률이 아님. 고배율은 여전히 큰 메모리/시간 비용 |
| native 캡처 | 기존256×192 경로와 별도 표시 버퍼 유지, 기존1~3배 SPIR-V 96개 바이트 동일 | native 실기 정확성·고배율의 실기 동일성은 별도 판정 |
| 표시 수명 | 1.1.127의 배율 변경·paused image·GL 할당 실패/재시도 처리 유지 | 실제 장기 게임·surface/device loss·다른 GPU 수락은 남음 |
| AD-04 | 1.1.127 pending-open 소유권 수정 유지, 이번 오디오/상태 복원32개 통과 | 계수/음질을 변경하지 않음. 물리 endpoint/청취·지연은 이번에 재검사하지 않음 |
| BV-04 | headless에서 없는 Qt graph 타깃을 등록하지 않도록 수정 | headless 전체 앱/전체 suite 통과를 의미하지 않음 |

Windows x64, CMake4.4.3/GCC16.2.0, RTX5080에서 관련114개 중111개가 통과했고 Qt offscreen 때문에3개가 skip됐다.
그3개를 native Qt 창 환경에서 재실행해3/3 통과했다. 새 셰이더512개 검증도 실패0이다. 자세한 조건과 원래 skip 기록은 릴리스 문서를 따른다.
Classic/Software/Compute/Vulkan 중 하나를 전역 정답으로 취급하지 않는다. 화면 확대와 실기 정확성은 다른 축이다.
2D 스프라이트/배경의 원본 디테일은 native다. 16배 표시 크기는 화면당4096×3072이지만 모든 GPU의 실행을 보장하지 않는다.

## 다음 작업 — 확인한 코드 경계에 따른 순서

| 과제 | 조사/구현 지점 | 다음 완료 조건 |
|---|---|---|
| GR-08 native 화면 차이 | 동일 입력의 3D 결과→2D 합성→capture→표시에서 최초 차이 추출 | 공개 fixture/실기 관측으로 기대값 확정, 허용 오차 확대 없이 회귀 |
| GR-14 GL scratch | `GPU3D_Compute.cpp`는 MaxBatchWork보다 큰 MaxWorkTiles 기준으로 임시 버퍼 할당 | 보존된 split-batch 메모리 RED를 수정하고 색/깊이/그림자·분할 결과 유지 |
| GR-14/15 전송 비용 | `ComputePipeline::UploadImage`의 매 업로드 staging 할당·SubmitAndWait | 호출량/대기 계측 후 재사용·묶음 제출 A/B. guest 소비 시점 뒤로 미루지 않음 |
| GR-14/15 host 비용 | `ComputePipeline::Render`의 결과 vector 복사, `GPU3D_Vulkan::DrawFrame`의 매 프레임 준비 배열 | 재사용 수명/실패 복구를 정의하고 출력·지연·메모리 비용 비교 |
| GR-15 pipeline/배율 | `ComputePipeline::Init`가32개 pipeline을 생성하며 cache 인자는 비어 있음. 8→9배는 tile 전환으로 scratch가 크게 증가 | 초기/반복 배율 전환 비용 측정 후 cache·자원 준비 정책 선택. 다른 tile 구성을 바로 정답으로 채택하지 않음 |
| GR-15 GPU 합성·직접 표시 | 현재 CPU 2D 합성과 RAM 표시 업로드 | native capture 가시성·frame 수명·복구를 먼저 고정하고 별도 작은 단계로 개발 |
| AD-04 오디오 | callback drain/마지막 join, 장치 탈착·장기 공급 부족, Minimum-phase 부하 | 현재32개 회귀와 실제 청취/물리 지연 결과를 구분. 살아 있는 callback을 detach하지 않음 |
| BV-07 설명/도구체인 | 해상도 `.ui` 도움말의1~3배 문구, TOML literal 및 CMake 정책 경고 | 실제 선택 범위와 설명 정합성, 경고 원인별 수정. 버전 번호만 올리는 변경 금지 |

GL scratch는 메모리 조건의 반례만 확인한 미적용 후속이다. 추가 수정 요청이 도구에서 차단돼 시험 diff와 실패 로그를 로컬에 보존하고 일반 테스트는 복원했다.
Vulkan 자원 최적화와 GL 미적용 후보를 합쳐 구현 완료로 표현하지 않는다.
현재 장치에서의 제한된 합성 GPU 검증이며 실제 게임/DSi·다른 GPU/OS·장기 부하는 계속 열려 있다. Actions·macOS 빌드는 사용하지 않는다.
[과제 목록](Task_Catalog.md), [원래 버전 계획](Release_Plan.md), [오디오 상태](Audio_Status.md), [검증 이력](Validation.md)의 원 과제 ID와 과거 실행 기록은 유지한다.

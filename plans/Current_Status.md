# 현재 개발 상태 — 1.1.130

2026-09-16. 기준은 1.1.129 `a3b7880d`다. [1.1.130 구현·측정·검증](releases/1.1.130.md)을 우선한다.
이전 1.1.03~1.1.44는 원래 책임 배정이며 현재 제품 버전 상한이나 모든 과제 완료 목록이 아니다.

| 영역 | 현재 구현 | 남은 범위 |
|---|---|---|
| GR-14 GL Compute | 1.1.129의 실제 배치 한도 기반 scratch와 일치하는 배율 한도 검사 유지 | 다른 GPU의 한도·실제 게임 비용 |
| GR-15 Vulkan 전송 | 1.1.129의 동기 staging·CPU 결과 버퍼 재사용 유지 | texture 업로드 묶음 제출·비동기화 |
| GR-14/15 readback | HOST_CACHED 우선, 필수 coherent/visible 및 허용 type mask 유지, 메모리 부족 fallback | 다른 GPU·메모리 구성·실게임에서의 성능 수락 |
| GR-15 pipeline | 같은 Device의 transient VkPipelineCache 재사용, 선택적 캐시 OOM fallback | cold 시작·실제 배율 전환 비용·다른 드라이버의 상주량 |
| D-004 표시 | 1~16배 실제 확대 3D와 native 캡처 분리 유지 | GPU 2D/capture와 직접 GPU 표시 |
| BV-07 정리 | 1~16배 도움말, TOML char8_t literal 표기, Teakra 진단 설정 및 작은 C++/Qt 경고 정리 | 모든 툴체인/OS의 경고 없는 clean build를 의미하지 않음 |
| AD-04 오디오 | 기존 종료 소유권 수정 유지, 이번 PCM·필터·보간 계수 변경 없음 | 장기 청취·물리 지연·장치 탈착·영구 정지 드라이버 |

이번 readback 측정은 clear frame·전체 결과 회수·색 변환·checksum 소비의 합성 workload다.
측정된 개선율을 실제 게임 FPS로 확대하지 않는다. 기존 1.1.129 코드가 우선 master에 게시됐고 이번 수정은 그 위에 쌓인다.
Classic·Software·Compute·Vulkan의 일치와 DS/DSi 실기 정답은 다르다. 배율 증가도 정확성의 근거가 아니다.

## 이어서 할 작업과 완료 기준

1. **Native 화면 불일치:** 동일 입력의 3D 결과→2D 합성→guest capture→표시 중 최초 차이와 최소 반례를 확보한다. 독립 시험/실기 관측 없이 한 렌더러를 전역 정답으로 삼지 않는다.
2. **업로드 묶음:** clear color/depth의 제출2회를 재현했다. 구현 쓰기가 차단돼 아직 미적용이다. 공개 API 반환 시 완료·성장 실패 복구·동일 이미지 및 제출 수 감소를 함께 검증해야 한다.
3. **GPU 합성·직접 표시:** scanline별 register/window/FIFO와 CPU/DMA capture 가시성, 이전 프레임 수명·장치 실패를 먼저 고정한다. 현재 CPU 합성 제거가 끝난 것은 아니다.
4. **메모리와 cache:** 실게임 cold/warm 및 배율 왕복, 8→9배 타일 전환의 자원 비용, AMD/Intel·통합 GPU에서 각각 확인한다. HOST_CACHED가 없으면 정확한 fallback은 가능하지만 같은 성능 이득은 보장하지 않는다.
5. **오디오:** 회귀·무음 endpoint 시험과 장기 청취/물리 지연/탈착을 구분한다. 살아 있는 callback이나 driver 작업을 detach하여 종료 문제를 숨기지 않는다.

원 과제 ID와 과거 실행 기록은 [Task_Catalog](Task_Catalog.md), [Release_Plan](Release_Plan.md), [Validation](Validation.md), [Audio_Status](Audio_Status.md)에 유지한다.
Actions·macOS 빌드·개인 ROM/BIOS/NAND/저장 파일 사용은 하지 않는다. 다른 작업트리의 체크아웃 파일은 유지한다.

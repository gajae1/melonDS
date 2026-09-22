# 최적화 후보 평가 — 2026-09-22

기준은 1.1.159 `74ed701a`와 당시 진행 중인 1.1.160 변경이다.
12개 영역을 병렬 조사해 구체적인 실험 후보 42개와 보류·기각·기반영 항목을
구분했다. 아래 후보는 현재 코드에서 반복 작업을 확인한 **측정 전 제안**이다.
42개의 속도 개선을 달성했다는 뜻이 아니며, 모든 소스 줄에 대한 정확성 감사도 아니다.
1.1.160의 실제 채택·검증 결과는 [릴리스 기록](releases/1.1.160.md)을 따른다.

## 영역별 판단

| 영역 | 후보 수 | 구체 후보 | 적용 조건과 우선순위 |
|---|---:|---|---|
| CPU/JIT | 3 | x64 Thumb의 dead carry 계산 제거; register-cache suffix 분석 재사용; interpreter 다중 전송의 선택 레지스터 순회 | 첫 후보 우선. carry 입력·출력 구분, 경계 시프트·CPSR·cycle·native 재진입 대조. suffix는 컴파일 시간에만 영향 |
| 메모리·DMA·scheduler | 3 | scheduler 활성 비트 순회; 동일 MPU 설정 재계산 생략; DS MainRAM 전용 DMA 경로 | scheduler부터. callback 중 취소·교체·신규 등록과 같은 시각의 ID 순서 보존. DMA는 겹침·SMC·단위별 timing 때문에 후순위 |
| Software 2D | 5 | 비모자이크 text BG 타일 구간 처리; capture 합성의 불변 후보 준비; post-mosaic OBJ 우선순위 분류; window 구간 덮어쓰기; native capture blend 채널 연산 | BG/OBJ는 기존 측정에서 남은 비용이 큰 영역. 타일 구간 우선. 이미 타일마다 묶인 tilemap 읽기를 절감 효과로 중복 계산하지 않음 |
| Geometry·Software 3D | 4 | Z 탈락 픽셀·shadow-mask의 불필요한 원근 나눗셈 제거; 완전히 내부인 polygon clip fast path; 행별 활성 polygon 순회; fog/AA의 정확한 채널 묶음 계산 | 원근 나눗셈부터. W-buffer·AA 두 깊이·shadow stencil 및 정수 반올림 보존. geometry와 raster 작성자 분리 가능 |
| OpenGL·공통 texcache | 4 | decode 내부 색 변환 재사용; capture downscale의 필요한 행만 처리; 무시되는 color-zero 키 비트 정규화; 동일 shader-config UBO 전송 생략 | 압축 4색 변환부터. 작은 indexed texture에서 table 준비 비용이 커질 수 있어 일괄 도입 금지. capture 행 제한은 GPU 상태·가시성 검증 필요 |
| Compute·Vulkan | 3 | 실제 span 수만큼 setup index 준비; 중복 indirect-buffer bind 제거; cached native readback landing 직접 소비 | 첫 후보는 CPU 준비 비용에 한정. 대규모 Vulkan 투자는 계속 보류. dispatch·barrier·native/enhanced 분리 유지 |
| 오디오 core | 3 | dense block별 나눗셈을 mixer-grid 차이로 대체; 값·period 불변의 내부 관측 병합; ADPCM magnitude 표 | dense 나눗셈 우선. PCM·capture·저장 상태·합산 순서·처리 중 할당 및 dense/sparse 대조. 계수·음질·기본값 유지 |
| DSP·주변장치 | 3 | 무동작 BTDMP callback 등록 제거; camera 행 좌표 나눗셈 제거; 고정 enum의 hash 조회 제거 | BTDMP부터 실제 LLE fixture로 확인. 기존 NOP fixture는 해당 peripheral callback을 포함하지 않으므로 성능 근거로 부적합 |
| 저장·카트리지 | 4 | SaveManager dirty 구간 게시; state-load staging 중복 초기화 제거; FAT mount 내 export 판정 재사용; 기존 동기 FAT 요청 안에서 NAND write 묶음 | 저장 writer snapshot·원자적 교체·실패 복구 유지. dirty 게시가 생산 스레드 복사량을 줄일 후보이며 실제 지연 효과는 미측정 |
| 통신 | 2 | Slirp 빈 descriptor·0 timeout의 OS poll만 생략; DSi 진단 hex dump를 행으로 묶음 | 타이머·protocol 처리는 유지. 게임 CPU 우선순위는 낮음. DNS 비동기화·packet batching은 별도 수명·순서 근거 필요 |
| frontend | 4 | 동일 DisplayFrame의 소유 복사본 재사용; Qt native update 병합; SDL2 topology 조회 할당 제거; camera 동일 크기 변환·짧은 게시 | SDL2는 작은 독립 후보. DisplayFrame은 generation뿐 아니라 NDS·context·extent·preserved-image 전이를 검증. camera는 아래 정확성 선행 과제 이후 |
| 빌드·도구 | 4 | 명시적 LTO OFF의 IPO probe 생략; ZIP 중복 압축 해제 검증 통합; coefficient 바이트의 compiler parsing 감소; PE 검사 batch | 개발·패키징 시간 후보이며 게임 FPS와 구분. CRC·SHA·파일 집합·source identity 검증은 유지. LTO/PGO 기본 승격 근거 없음 |

## 다음 구현 묶음

첫 묶음은 아래 독립 소유 파일로 진행할 수 있다. 각 후보는 원본과 출력·상태를
대조하고, 효과가 반복 측정되며 대조 부하 회귀가 없을 때만 채택한다.
소스상 연산 수 감소나 통과한 테스트 수만으로 성능 성공을 선언하지 않는다.

| 순서 | 작업 | 단일 작성자의 production 범위 | 결정에 필요한 근거 |
|---|---|---|---|
| A1 | dense mixer-grid indexing | `src/AudioInterpolationStream.h` | dense·sparse·mixed의 PCM/상태 동일, 반복 dense 시간 감소, sparse 대조 |
| A2 | 압축 texture decode의 4색 변환 | `src/GPU3D_Texcache.cpp` | 모든 compressed mode·투명도·wrap의 바이트 동일, 작은/큰 miss 비용 |
| A3 | x64 Thumb dead carry 생략 | `src/ARMJIT_x64/ARMJIT_ALU.cpp` | interpreter/native의 register·CPSR·cycle 동일, live carry/RRX·block exit 대조 |
| A4 | scheduler 활성 비트 순회 | `src/NDS.cpp` | callback mutation·sleep 순서 동일, sparse/dense/낮은 ID 대조 |
| A5 | BTDMP 무동작 callback 제거 | `src/teakra/src/btdmp.cpp` | 실제 Teakra timer/IRQ/I2S 상태·PCM 동일, LLE cycle 처리 비용 |
| A6 | software Z 원근 계산 지연 | `src/GPU3D_Soft.cpp`, `.h` | color/depth/attribute/stencil 동일, Z overdraw·W-buffer·equal-W 대조 |

A1–A6은 production 파일이 겹치지 않는다. 테스트 등록·통합 빌드·배포는 한 작성자가
맡고 성능 측정은 직렬 실행한다. 기존 dirty `test_generator.cpp`는 보존한다.
후속 묶음은 text BG 타일 처리, SaveManager 게시, SDL2 topology 조회, native 표시
복사 재사용을 우선한다. 같은 GPU2D 파일의 여러 후보는 순차 적용한다.
디스크 I/O·카메라·통신은 해당 부하가 없는 DS 게임의 FPS 향상으로 포장하지 않는다.

## 정확성 선행 과제

frontend 조사에서 다음 소스 경로가 확인됐다. 아직 실행 재현된 결함으로 확정하지
않았으며, camera 최적화의 비교 원본으로 그대로 삼아서는 안 된다.

- Qt camera 입력이 각 plane의 `bytesPerLine`을 전달하지 않아 padded stride를
  tight packing으로 처리할 가능성: `CameraManager.cpp`의 frame 전달·변환 경로.
- still image를 변환한 `imgconv` 대신 원래 `img.bits()`를 사용하는 경로.
- device 재초기화 중 `startNum` 초기화가 active 상태의 재시작 요청을 잃는 경로.

생성된 padded UYVY/NV12, RGB888/indexed 이미지, active→missing→present 상태로
각각 재현하고 수정한다. 실제 장치 탈착·청취는 별도 수락 경계로 남긴다.

## 보류 및 재시도하지 않을 것

- 1.1.159에서 이득이 입증되지 않은 JIT recovered-block 직접 실행,
  response-index cache·stereo SIMD 후보를 다시 배포하지 않는다.
- 높은 thread priority·swap 억제는 저버퍼 shortage 해결 효과가 반복되지 않았다.
  프레임 생략, 기본 버퍼 확대, 필터 품질 변경으로 성능 검사를 통과시키지 않는다.
- GPU 직접 표시·여러 프레임 비동기 처리, 광범위한 SIMD/tiling, DMA/MMIO memcpy,
  event tick 묶음은 ownership·visibility·guest timing 근거 없이 적용하지 않는다.
- 기존 캐시·dirty hash·brightness·readback·오디오 잠금 개선은 이미 반영됐다.
  오래된 workstream의 제안을 현재 미구현 항목으로 다시 세지 않는다.

실게임 FPS·Qt 표시와 저버퍼의 지속 공급·다른 CPU/드라이버·물리 장치 수락을 포함한
전체 목표는 계속 진행 중이다. 이 평가는 다음 실험의 순서와 범위를 정한 결과다.

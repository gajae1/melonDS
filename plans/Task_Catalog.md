# 과제 목록

125개 과제 ID를 유지한다. 이는 확정 버그 수나 독립 구현 수가 아니다. P1은 재현 우선, P2는 호환성·측정 개선, P3은 효용 확인 후 채택이다. 관찰 근거는 1.1.02 분석 당시 분류이며 실행으로 새로 확인한 항목은 처리 상태를 갱신한다.

기본 버전은 책임 범위다. 작은 수정의 선반영은 [릴리스 기록](releases/1.1.12.md)에 원 ID와 남은 범위를 남긴다. 다른 ID와 같은 경계를 공유하면 하나의 구현으로 집계한다.

| ID | 작업·상세 계약 | 관찰 근거 | 우선 | 기본 버전 | 공동 대표 | 처리 상태 |
|---|---|---|---|---|---|---|
| CJ-01 | [A64 MUL 가변 사이클](workstreams/01-core-jit.md#CJ-01) | 정적 후보 | P1 | [1.1.39](Release_Plan.md#v39) | — | 미착수 |
| CJ-02 | [A64 조건부 사이클 누적](workstreams/01-core-jit.md#CJ-02) | 정적 후보 | P1 | [1.1.39](Release_Plan.md#v39) | — | 미착수 |
| CJ-03 | [warmed ALU·shift·부분 flags](workstreams/01-core-jit.md#CJ-03) | 관찰/확장 | P1 | [1.1.14](Release_Plan.md#v14) | — | 미착수 |
| CJ-04 | [MPU 실행·data 권한 경계](workstreams/01-core-jit.md#CJ-04) | 정적 후보 | P1 | [1.1.15](Release_Plan.md#v15) | — | 미착수 |
| CJ-05 | [DTCM remap 구간](workstreams/01-core-jit.md#CJ-05) | 정적 후보 | P1 | [1.1.15](Release_Plan.md#v15) | — | 미착수 |
| CJ-06 | [빈 LDM/STM reg list](workstreams/01-core-jit.md#CJ-06) | 정적 후보 | P2 | [1.1.14](Release_Plan.md#v14) | — | 미착수 |
| CJ-07 | [callback 중 event 취소](workstreams/01-core-jit.md#CJ-07) | 정적 후보 | P1 | [1.1.15](Release_Plan.md#v15) | — | 미착수 |
| CJ-08 | [IRQ·DMA·sleep 경계](workstreams/01-core-jit.md#CJ-08) | 관찰/확장 | P2 | [1.1.15](Release_Plan.md#v15) | — | 미착수 |
| CJ-09 | [savestate event 값 검증](workstreams/01-core-jit.md#CJ-09) | 정적 후보 | P1 | [1.1.05](Release_Plan.md#v05) | — | 1.1.03 구현·로컬 회귀; DSi HLE 실사용 후속 |
| CJ-10 | [부분 load 실패 복원](workstreams/01-core-jit.md#CJ-10) | 실행 재현 | P1 | [1.1.05](Release_Plan.md#v05) | FS-01 | 1.1.04 load·undo 복원·정지 구현, 합성 core/JIT 회귀; 카트·DSi·게임 수락 후속 |
| CJ-11 | [GDB SPSR bank 접근](workstreams/01-core-jit.md#CJ-11) | 정적 후보 | P1 | [1.1.10](Release_Plan.md#v10) | — | 미착수 |
| CJ-12 | [DMA 버스 타이밍](workstreams/01-core-jit.md#CJ-12) | 정적 후보 | P2 | [1.1.15](Release_Plan.md#v15) | — | 미착수 |
| CJ-13 | [ARM9 cache·ITCM 타이밍](workstreams/01-core-jit.md#CJ-13) | 관찰/확장 | P2 | [1.1.15](Release_Plan.md#v15) | — | 미착수 |
| CJ-14 | [DSi clock·NDMA 순서](workstreams/01-core-jit.md#CJ-14) | 정적 후보 | P2 | [1.1.20](Release_Plan.md#v20) | AD-16 | 미착수 |
| CJ-15 | [SMC·alias·remap 재진입](workstreams/01-core-jit.md#CJ-15) | 관찰/확장 | P1 | [1.1.15](Release_Plan.md#v15) | — | 미착수 |
| CJ-16 | [A64 native fastmem·W^X·I-cache](workstreams/01-core-jit.md#CJ-16) | 관찰/확장 | P1 | [1.1.39](Release_Plan.md#v39) | — | 미착수 |
| CJ-17 | [BMI BIC·shift 실효성](workstreams/01-core-jit.md#CJ-17) | 미측정 가설 | P3 | [1.1.25](Release_Plan.md#v25) | — | 미착수 |
| CJ-18 | [짧은 block hash·cache 비용](workstreams/01-core-jit.md#CJ-18) | 미측정 가설 | P3 | [1.1.25](Release_Plan.md#v25) | — | 미착수 |
| CJ-19 | [3D capture의 CPU/DMA 가시성](workstreams/01-core-jit.md#CJ-19) | 관찰/확장 | P2 | [1.1.16](Release_Plan.md#v16) | GR-06 | 미착수 |
| CJ-20 | [입력·RTC·다중 인스턴스 시간 정책](workstreams/01-core-jit.md#CJ-20) | 관찰/확장 | P2 | [1.1.03](Release_Plan.md#v03) | — | 미착수 |
| CJ-21 | [DSi 동일 조건 upstream 부팅 비교](workstreams/01-core-jit.md#CJ-21) | 관찰/확장 | P2 | [1.1.18](Release_Plan.md#v18) | AD-17 | 미착수 |
| GR-01 | [배율 변경의 pending capture 보존](workstreams/02-renderers.md#GR-01) | 정적 후보 | P1 | [1.1.16](Release_Plan.md#v16) | — | 미착수 |
| GR-02 | [Compute capture sampler unit](workstreams/02-renderers.md#GR-02) | 정적 후보 | P1 | [1.1.17](Release_Plan.md#v17) | — | 미착수 |
| GR-03 | [capability·shader 실패 전파](workstreams/02-renderers.md#GR-03) | 정적 후보 | P1 | [1.1.17](Release_Plan.md#v17) | — | 미착수 |
| GR-04 | [GL 객체 해제·실패 초기화](workstreams/02-renderers.md#GR-04) | 정적 후보 | P1 | [1.1.16](Release_Plan.md#v16) | — | 미착수 |
| GR-05 | [mid-capture 시점·register latch](workstreams/02-renderers.md#GR-05) | 정적 후보 | P1 | [1.1.16](Release_Plan.md#v16) | — | 미착수 |
| GR-06 | [실제 source A→guest readback](workstreams/02-renderers.md#GR-06) | 관찰/확장 | P1 | [1.1.16](Release_Plan.md#v16) | — | 미착수 |
| GR-07 | [고배율 한도·할당 복구](workstreams/02-renderers.md#GR-07) | 정적 후보 | P2 | [1.1.17](Release_Plan.md#v17) | — | 미착수 |
| GR-08 | [depth·fog·AA·edge 정확성](workstreams/02-renderers.md#GR-08) | 정적 후보 | P1 | [1.1.28](Release_Plan.md#v28) | — | 미착수 |
| GR-09 | [2D/3D 정수 SIMD·구간 묶음](workstreams/02-renderers.md#GR-09) | 미측정 가설 | P2 | [1.1.27](Release_Plan.md#v27) | — | 미착수 |
| GR-10 | [texture cache 비용·상주량](workstreams/02-renderers.md#GR-10) | 미측정 가설 | P2 | [1.1.28](Release_Plan.md#v28) | — | 미착수 |
| GR-11 | [surface 상실·표시 복구](workstreams/02-renderers.md#GR-11) | 정적 후보 | P2 | [1.1.16](Release_Plan.md#v16) | — | 미착수 |
| GR-12 | [다중 context 초기화 도달성](workstreams/02-renderers.md#GR-12) | 정적 후보 | P2 | [1.1.16](Release_Plan.md#v16) | — | 미착수 |
| GR-13 | [software worker 공개·중단 수명](workstreams/02-renderers.md#GR-13) | 정적 후보 | P2 | [1.1.28](Release_Plan.md#v28) | — | 미착수 |
| GR-14 | [render·readback·present 비용 분해](workstreams/02-renderers.md#GR-14) | 미측정 가설 | P2 | [1.1.30](Release_Plan.md#v30) | — | 미착수 |
| GR-15 | [Vulkan 전체 backend 신설](workstreams/02-renderers.md#GR-15) | 기능 부재 관찰·이득 미측정 | P3 | [1.1.32](Release_Plan.md#v32) | — | 미착수 |
| GR-16 | [frame 종류·수명·반납 계약](workstreams/02-renderers.md#GR-16) | 관찰/확장 | P3 | [1.1.32](Release_Plan.md#v32) | — | 미착수 |
| AD-01 | [마이크 보간 끝 sample 경계](workstreams/03-audio-dsi.md#AD-01) | 실행 재현 | P1 | [1.1.08](Release_Plan.md#v08) | — | 1.1.07 마지막 sample·빈 입력 보호와 생성 파형/보호 페이지 회귀; 실제 장치 수락 후속 |
| AD-02 | [마이크 공유 count·장치 수명](workstreams/03-audio-dsi.md#AD-02) | 실행 재현 | P1 | [1.1.08](Release_Plan.md#v08) | — | 1.1.07 count 잠금·FPS 게시·열린 장치 재호출 보존 회귀; 실제 재연결·다중 인스턴스·TSan 후속 |
| AD-03 | [최초 SPU bitdepth 정책](workstreams/03-audio-dsi.md#AD-03) | 정적 후보 | P1 | [1.1.09](Release_Plan.md#v09) | — | 미착수 |
| AD-04 | [underrun·지연·장치 복구](workstreams/03-audio-dsi.md#AD-04) | 미측정 가설 | P2 | [1.1.08](Release_Plan.md#v08) | — | 미착수 |
| AD-05 | [load/reset 오디오 이력 정책](workstreams/03-audio-dsi.md#AD-05) | 정적 후보 | P2 | [1.1.08](Release_Plan.md#v08) | — | 미착수 |
| AD-06 | [SPU capture 소스·가산](workstreams/03-audio-dsi.md#AD-06) | 관찰/확장 | P2 | [1.1.09](Release_Plan.md#v09) | — | 미착수 |
| AD-07 | [one-shot hold 동작](workstreams/03-audio-dsi.md#AD-07) | 관찰/확장 | P2 | [1.1.09](Release_Plan.md#v09) | — | 미착수 |
| AD-08 | [ADPCM loop·보간 oracle](workstreams/03-audio-dsi.md#AD-08) | 미측정 가설 | P2 | [1.1.09](Release_Plan.md#v09) | — | 미착수 |
| AD-09 | [modcrypt dev key 초기화](workstreams/03-audio-dsi.md#AD-09) | 정적 후보 | P1 | [1.1.18](Release_Plan.md#v18) | — | 미착수 |
| AD-10 | [modcrypt subarea offset](workstreams/03-audio-dsi.md#AD-10) | 정적 후보 | P2 | [1.1.18](Release_Plan.md#v18) | — | 미착수 |
| AD-11 | [SD producer/FIFO/sector 길이](workstreams/03-audio-dsi.md#AD-11) | 정적 후보 | P1 | [1.1.19](Release_Plan.md#v19) | — | 미착수 |
| AD-12 | [SD backing I/O 실패 전파](workstreams/03-audio-dsi.md#AD-12) | 정적 후보 | P1 | [1.1.19](Release_Plan.md#v19) | — | 미착수 |
| AD-13 | [NAND metadata exact read](workstreams/03-audio-dsi.md#AD-13) | 정적 후보 | P1 | [1.1.19](Release_Plan.md#v19) | — | 미착수 |
| AD-14 | [AES CCM FIFO tag 모드](workstreams/03-audio-dsi.md#AD-14) | 관찰/확장 | P2 | [1.1.29](Release_Plan.md#v29) | — | 미착수 |
| AD-15 | [AES/SD 완료 event 타이밍](workstreams/03-audio-dsi.md#AD-15) | 관찰/확장 | P2 | [1.1.20](Release_Plan.md#v20) | — | 미착수 |
| AD-16 | [NDMA arbitration·subblock](workstreams/03-audio-dsi.md#AD-16) | 관찰/확장 | P2 | [1.1.20](Release_Plan.md#v20) | — | 미착수 |
| AD-17 | [DSi boot/reset·SCFG/NWRAM](workstreams/03-audio-dsi.md#AD-17) | 관찰/확장 | P2 | [1.1.18](Release_Plan.md#v18) | — | 미착수 |
| AD-18 | [DSP modulo −1 실기 판정](workstreams/03-audio-dsi.md#AD-18) | 기존 실패 기록 | P1 | [1.1.21](Release_Plan.md#v21) | — | 미착수 |
| AD-19 | [DSP retd·vtrshr 지연](workstreams/03-audio-dsi.md#AD-19) | 관찰/확장 | P2 | [1.1.22](Release_Plan.md#v22) | — | 미착수 |
| AD-20 | [DSP AHBM/DMA·FIFO/IRQ](workstreams/03-audio-dsi.md#AD-20) | 관찰/확장 | P2 | [1.1.22](Release_Plan.md#v22) | — | 미착수 |
| AD-21 | [HLE/LLE·I2S/mic clock](workstreams/03-audio-dsi.md#AD-21) | 미측정 가설 | P2 | [1.1.22](Release_Plan.md#v22) | — | 미착수 |
| AD-22 | [I2C ACK·BPTWL reset](workstreams/03-audio-dsi.md#AD-22) | 관찰/확장 | P2 | [1.1.23](Release_Plan.md#v23) | — | 미착수 |
| AD-23 | [카메라 형식·sensor arbitration](workstreams/03-audio-dsi.md#AD-23) | 관찰/확장 | P2 | [1.1.23](Release_Plan.md#v23) | — | 미착수 |
| AD-24 | [AES block backend 실효성](workstreams/03-audio-dsi.md#AD-24) | 미측정 가설 | P3 | [1.1.29](Release_Plan.md#v29) | — | 미착수 |
| AD-25 | [SPU 정수 보간·pan 가속](workstreams/03-audio-dsi.md#AD-25) | 미측정 가설 | P3 | [1.1.09](Release_Plan.md#v09) | — | 미착수 |
| AD-26 | [DSP opcode/상태 coverage](workstreams/03-audio-dsi.md#AD-26) | 관찰/확장 | P2 | [1.1.22](Release_Plan.md#v22) | — | 미착수 |
| FS-01 | [load/undo 실패의 세션 복원](workstreams/04-frontend-storage.md#FS-01) | 실행 재현 | P1 | [1.1.05](Release_Plan.md#v05) | — | 1.1.04 CJ-10과 공동 구현·회귀; 전체 게임·장치 수락 후속 |
| FS-02 | [길이·전체 read·적용 전 검증](workstreams/04-frontend-storage.md#FS-02) | 실행 재현 | P1 | [1.1.07](Release_Plan.md#v07) | — | 1.1.04 상태·1.1.05 SRAM·1.1.06 ROM/import 구현·로컬 회귀 확인; 다른 부가 파일 후속 |
| FS-03 | [ROM 교체와 save 소유권](workstreams/04-frontend-storage.md#FS-03) | 실행 재현 | P1 | [1.1.04](Release_Plan.md#v04) | — | 1.1.05 준비 실패 보존·queued 카트·save 연결 구현·로컬 회귀; 실제 게임·DSi 수락 후속 |
| FS-04 | [손상 TOML 보존·오류 전파](workstreams/04-frontend-storage.md#FS-04) | 정적 후보 | P1 | [1.1.07](Release_Plan.md#v07) | — | 1.1.03 원본 보존·회귀; 상세 오류 UI 후속 |
| FS-05 | [save worker/path/buffer 수명](workstreams/04-frontend-storage.md#FS-05) | 잠금 경계 재현 | P1 | [1.1.04](Release_Plan.md#v04) | — | 1.1.04 mutex 통일·worker/파일 회귀; 느린 I/O·장기 경쟁 수락 후속 |
| FS-06 | [종료 시 지속 저장 실패](workstreams/04-frontend-storage.md#FS-06) | 실행 재현 | P1 | [1.1.04](Release_Plan.md#v04) | — | 1.1.05 pending·복구 API·종료 선택 구현·로컬 회귀; 실제 저장장치·장기 수락 후속 |
| FS-07 | [archive EOF/누락/오류·자원](workstreams/04-frontend-storage.md#FS-07) | 실행 재현 | P1 | [1.1.07](Release_Plan.md#v07) | — | 1.1.06 입력·자원·전체 읽기 구현·로컬 회귀 확인; 전체 형식·대용량 수락 후속 |
| FS-08 | [Zstd 실제 길이·frame 완료](workstreams/04-frontend-storage.md#FS-08) | 정적 후보 | P1 | [1.1.07](Release_Plan.md#v07) | — | 1.1.03 구현·회귀; 실제 최대 출력 경계 후속 |
| FS-09 | [controller 핸들·capability 정리](workstreams/04-frontend-storage.md#FS-09) | 실행 재현 | P1 | [1.1.06](Release_Plan.md#v06) | — | 1.1.07 공통 핸들/capability/rumble 정리·가상 SDL 장치 회귀; 실물 hotplug·motion 수락 후속 |
| FS-10 | [touch 좌표·cancel·공유 snapshot](workstreams/04-frontend-storage.md#FS-10) | 실행 재현 | P1 | [1.1.06](Release_Plan.md#v06) | — | 1.1.07 현재 좌표·취소/포커스 해제·원자적 좌표 게시 회귀; 실물·DPI/회전·다중 창 후속 |
| FS-11 | [DPI·다중 창·종료 경계](workstreams/04-frontend-storage.md#FS-11) | 관찰/확장 | P2 | [1.1.06](Release_Plan.md#v06) | — | 미착수 |
| FS-12 | [장치 identity·인스턴스 배정](workstreams/04-frontend-storage.md#FS-12) | 정적 후보 | P2 | [1.1.06](Release_Plan.md#v06) | — | 미착수 |
| FS-13 | [동일 basename save 충돌](workstreams/04-frontend-storage.md#FS-13) | 정적 확인·동작 검증 | P1 | [1.1.04](Release_Plan.md#v04) | — | 1.1.10 DS/GBA/member 정체성·기존/별도/취소 선택·사용자 공용 기록·고정 활성 경로; 생성 파일·Qt·코어 회귀, 실제 저장 재로드/이관 후속 |
| FS-14 | [RTC 파일 읽기·원자 저장](workstreams/04-frontend-storage.md#FS-14) | 실행 재현 | P1 | [1.1.13](Release_Plan.md#v13) | — | 1.1.06 파일 경계·원자 쓰기 구현·로컬 회귀 확인; 독립 파일 정책·날짜/게임 수락 후속 |
| FS-15 | [치트 편집 저장 실패](workstreams/04-frontend-storage.md#FS-15) | 실행 재현 | P1 | [1.1.13](Release_Plan.md#v13) | — | 1.1.12 편집 사본·직렬화 사전 검사·QSaveFile commit·재시도/유지/취소·실행 목록 갱신 회귀; 실제 게임 적용·전원 손실 수락 후속 |
| FS-16 | [DSi title 교체 rollback](workstreams/04-frontend-storage.md#FS-16) | 정적 후보 | P1 | [1.1.19](Release_Plan.md#v19) | — | 미착수 |
| FS-17 | [LAN/Netplay UI 연결 수명](workstreams/04-frontend-storage.md#FS-17) | 관찰/확장 | P2 | [1.1.12](Release_Plan.md#v12) | — | 미착수 |
| FS-18 | [GDB UI/코어 종료 경계](workstreams/04-frontend-storage.md#FS-18) | 관찰/확장 | P2 | [1.1.10](Release_Plan.md#v10) | — | 미착수 |
| FS-19 | [DS/DSi 설정·부팅 진단](workstreams/04-frontend-storage.md#FS-19) | 미측정 가설 | P2 | [1.1.18](Release_Plan.md#v18) | — | 미착수 |
| FS-20 | [느린 ROM 준비·취소](workstreams/04-frontend-storage.md#FS-20) | 미측정 가설 | P2 | [1.1.07](Release_Plan.md#v07) | — | 미착수 |
| FS-21 | [카메라 미리보기·hotplug 수명](workstreams/04-frontend-storage.md#FS-21) | 관찰/확장 | P2 | [1.1.23](Release_Plan.md#v23) | — | 미착수 |
| NP-01 | [LAN payload·AID·peer 검증](workstreams/05-connectivity-peripherals.md#NP-01) | 실행 재현 | P1 | [1.1.12](Release_Plan.md#v12) | — | 1.1.08 실제 ENet payload·type·sender/peer·AID 검증과 27개 생성 패킷 회귀; 빈 AID0 reply 보존, 두 PC 게임 수락 후속 |
| NP-02 | [LAN handshake·취소·소유권](workstreams/05-connectivity-peripherals.md#NP-02) | 정적 후보 | P1 | [1.1.12](Release_Plan.md#v12) | — | 미착수 |
| NP-03 | [LocalMP FIFO 넘침·복구](workstreams/05-connectivity-peripherals.md#NP-03) | 정적 후보 | P1 | [1.1.12](Release_Plan.md#v12) | — | 미착수 |
| NP-04 | [LocalMP 그룹 격리](workstreams/05-connectivity-peripherals.md#NP-04) | 관찰/확장 | P2 | [1.1.12](Release_Plan.md#v12) | — | 미착수 |
| NP-05 | [PCap library 실패 중복 해제](workstreams/05-connectivity-peripherals.md#NP-05) | 실행 재현 | P1 | [1.1.11](Release_Plan.md#v11) | — | 1.1.08 누락 심볼 시 단일 unload·이동/소유권 회귀; 실제 DLL·POSIX 수락 후속 |
| NP-06 | [PCap caplen/link type·I/O](workstreams/05-connectivity-peripherals.md#NP-06) | 실행 재현 | P1 | [1.1.11](Release_Plan.md#v11) | — | 1.1.08 완전한 Ethernet 캡처·datalink·I/O 오류와 open/열거 자원 회귀; 실제 어댑터·driver 실패 수락 후속 |
| NP-07 | [Slirp IPv4/UDP/DNS 경계](workstreams/05-connectivity-peripherals.md#NP-07) | 실행 재현 | P1 | [1.1.11](Release_Plan.md#v11) | — | 1.1.08 IPv4/UDP 길이·옵션·DNS 단일 A/IN 질문 검증 후 조회; 생성 응답/보호 페이지 회귀, 실제 DS DNS 수락 후속 |
| NP-08 | [DS/DSi Wi-Fi 실패 event](workstreams/05-connectivity-peripherals.md#NP-08) | 관찰/확장 | P2 | [1.1.12](Release_Plan.md#v12) | — | 미착수 |
| NP-09 | [Netplay host/client 실제 게임](workstreams/05-connectivity-peripherals.md#NP-09) | 관찰/확장 | P2 | [1.1.31](Release_Plan.md#v31) | — | 미착수 |
| NP-10 | [GDB packet/framing 용량](workstreams/05-connectivity-peripherals.md#NP-10) | 실행 재현 | P1 | [1.1.10](Release_Plan.md#v10) | — | 1.1.08 framing/escape/checksum·응답 용량·불량 패킷 뒤 회복 회귀; 정확히 찬 수신 버퍼의 NUL 제거는 정적 결함 수정 |
| NP-11 | [GDB M/X 파싱·무부분쓰기](workstreams/05-connectivity-peripherals.md#NP-11) | 실행 재현 | P1 | [1.1.10](Release_Plan.md#v10) | — | 1.1.08 m/M/X 범위·정확한 본문 길이·escape를 전부 검증한 뒤 쓰기; 기존 MMIO 폭 보존, 실제 ARM debugger 수락 후속 |
| NP-12 | [GDB 재접속·EOF·부분 송신](workstreams/05-connectivity-peripherals.md#NP-12) | 실행 재현 | P1 | [1.1.10](Release_Plan.md#v10) | — | 1.1.08 SOCKET 폭·EOF·부분/0-byte 전송·NoAck 초기화, 실제 Windows loopback 분할/재접속/해제 확인; Linux·양 CPU debugger 후속 |
| NP-13 | [IR 미초기화 응답·복원](workstreams/05-connectivity-peripherals.md#NP-13) | 실행 재현 | P1 | [1.1.24](Release_Plan.md#v24) | — | 1.1.10 FF 호환성 fallback·IRPos/reset/14.1 상태·14.0 새 명령 경계·section 검증; 실제 IR 통신/응답값 후속 |
| NP-14 | [BT 키보드 최소 protocol](workstreams/05-connectivity-peripherals.md#NP-14) | 관찰/확장 | P2 | [1.1.24](Release_Plan.md#v24) | — | 미착수 |
| NP-15 | [카트리지 슬롯/IRQ/DRQ 타이밍](workstreams/05-connectivity-peripherals.md#NP-15) | 부분 실행 재현 | P2 | [1.1.24](Release_Plan.md#v24) | — | 1.1.11 AUXSPI mask 뒤 실제 mode 전이 비교, ARM9/ARM7 MMIO·busy·CS·소유권·상태 복원 회귀; 속도/전원/DRQ 타이밍 후속 |
| NP-16 | [homebrew argv·DLDI span](workstreams/05-connectivity-peripherals.md#NP-16) | 실행 재현 | P1 | [1.1.24](Release_Plan.md#v24) | — | 1.1.09 argv 용량·주소, DLDI 실제 ARM9/source/target/fixup 범위와 RW/RO·재패치 회귀; 실제 홈브루 FAT/부팅 후속 |
| NP-17 | [GBA sensor/rumble/RAM 계약](workstreams/05-connectivity-peripherals.md#NP-17) | 부분 실행 재현 | P2 | [1.1.24](Release_Plan.md#v24) | — | 1.1.10 진동 AD1 마스크·이전 상태·callback 회귀; 실제 pad 수명과 sensor/RAM 계약 후속 |
| NP-18 | [카트리지 save type·GBA 연동](workstreams/05-connectivity-peripherals.md#NP-18) | 부분 실행 재현 | P2 | [1.1.24](Release_Plan.md#v24) | — | 1.1.11 512B EEPROM 16B page wrap·저장 통지와 14.1 중간 복원 회귀; chip 판별·GBA ROM bus/DMA·WIP/WP·실기 후속 |
| NP-19 | [RTC 달력·IRQ·host 시계](workstreams/05-connectivity-peripherals.md#NP-19) | 부분 실행 재현 | P2 | [1.1.13](Release_Plan.md#v13) | — | 1.1.10 직렬 edge·분 carry·통신 Reset 수정, 달력/시간모드/BCD/IRQ·배터리 State 보존 회귀; host/DST·실기 초기값·게임 후속 |
| NP-20 | [치트 DB 길이·read 오류](workstreams/05-connectivity-peripherals.md#NP-20) | 실행 재현 | P1 | [1.1.13](Release_Plan.md#v13) | — | 1.1.09 header/index/string/code·read/seek 오류 검증, 완전한 entry만 반환·부모 포인터 보존·빈 선택 import 차단; 실제 DB 선택 후속 |
| NP-21 | [치트 literal/loop 실행 계약](workstreams/05-connectivity-peripherals.md#NP-21) | 실행 재현 | P1 | [1.1.13](Release_Plan.md#v13) | — | 1.1.09 빈/홀수/E 본문 사전 검증, UI 요청으로 loop/copy 취소·실행본 비활성화·오류 보고; 실제 게임·D1/D2 조건 복원·C5 수명 후속 |
| BV-01 | [기준 revision·검증 자료 색인](workstreams/06-build-validation.md#BV-01) | 관찰/확장 | P1 | [1.1.03](Release_Plan.md#v03) | — | 1.1.03 부분 반영: 공개 계획·증거 색인 |
| BV-02 | [다자릿수 표시/숫자 버전](workstreams/06-build-validation.md#BV-02) | 관찰/확장 | P1 | [1.1.43](Release_Plan.md#v43) | — | 미착수 |
| BV-03 | [Windows DLL/plugin 의존 폐쇄](workstreams/06-build-validation.md#BV-03) | 관찰/확장 | P1 | [1.1.43](Release_Plan.md#v43) | — | 1.1.03 부분 반영: import·독립 PATH 확인 |
| BV-04 | [로컬 build profile](workstreams/06-build-validation.md#BV-04) | 관찰/확장 | P1 | [1.1.43](Release_Plan.md#v43) | — | 1.1.03 부분 반영: 기존 로컬 명령 |
| BV-05 | [배포 allowlist·개인 자료 보존](workstreams/06-build-validation.md#BV-05) | 정적 후보 | P1 | [1.1.43](Release_Plan.md#v43) | — | 1.1.03 부분 반영: 선별한 배포 파일 |
| BV-06 | [C23/C++26 기능별 채택](workstreams/06-build-validation.md#BV-06) | 관찰/확장 | P2 | [1.1.36](Release_Plan.md#v36) | — | 미착수 |
| BV-07 | [의존성·local patch·license](workstreams/06-build-validation.md#BV-07) | 관찰/확장 | P1 | [1.1.36](Release_Plan.md#v36) | — | 미착수 |
| BV-08 | [LTO/ThinLTO guard 판정](workstreams/06-build-validation.md#BV-08) | 미측정 가설 | P2 | [1.1.38](Release_Plan.md#v38) | — | 미착수 |
| BV-09 | [PGO train/holdout](workstreams/06-build-validation.md#BV-09) | 미측정 가설 | P2 | [1.1.38](Release_Plan.md#v38) | — | 미착수 |
| BV-10 | [paired median/tail 측정](workstreams/06-build-validation.md#BV-10) | 관찰/확장 | P1 | [1.1.03](Release_Plan.md#v03) | — | 미착수 |
| BV-11 | [sanitizer·JIT fault 구분](workstreams/06-build-validation.md#BV-11) | 관찰/확장 | P2 | [1.1.03](Release_Plan.md#v03) | — | 미착수 |
| BV-12 | [CPU/OS feature·fallback](workstreams/06-build-validation.md#BV-12) | 관찰/확장 | P1 | [1.1.26](Release_Plan.md#v26) | — | 미착수 |
| BV-13 | [분리 symbol·진단자료](workstreams/06-build-validation.md#BV-13) | 관찰/확장 | P2 | [1.1.43](Release_Plan.md#v43) | — | 미착수 |
| BV-14 | [Linux/BSD native 수락](workstreams/06-build-validation.md#BV-14) | 장치/조건부 | P2 | [1.1.42](Release_Plan.md#v42) | — | 미착수 |
| BV-15 | [Windows/Linux ARM64 native](workstreams/06-build-validation.md#BV-15) | 장치/조건부 | P1 | [1.1.40](Release_Plan.md#v40) | — | 미착수 |
| BV-16 | [고급 ARM/Apple 조건부 gate](workstreams/06-build-validation.md#BV-16) | 장치/조건부 | P2 | [1.1.41](Release_Plan.md#v41) | — | 미착수 |
| BV-17 | [장기 게임·실기 통합 수락](workstreams/06-build-validation.md#BV-17) | 관찰/확장 | P1 | [1.1.44](Release_Plan.md#v44) | — | 미착수 |
| BV-18 | [upstream/provenance 유지](workstreams/06-build-validation.md#BV-18) | 관찰/확장 | P1 | [1.1.03](Release_Plan.md#v03) | — | 미착수 |
| BV-19 | [실험 기능 승격·복구 matrix](workstreams/06-build-validation.md#BV-19) | 관찰/확장 | P1 | [1.1.44](Release_Plan.md#v44) | — | 미착수 |
| BV-20 | [SDL3 API·장치 전환](workstreams/06-build-validation.md#BV-20) | 신규 이행 | P2 | [1.1.37](Release_Plan.md#v37) | — | 미착수 |

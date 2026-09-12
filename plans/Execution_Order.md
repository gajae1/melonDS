# 착수 순서

먼저 실제 소스와 후보를 대조하고 실패·정상 입력 한 쌍을 준비한다. 재현되면 최소 수정하고, 반증되면 정상 구현을 유지하며 근거를 남긴다.
우선순위는 발생률이나 완료 상태가 아니다. 다음 다섯 묶음은 책임 버전을 변경하지 않는 실행 순서다.

| 순서 | ID / 책임 버전 | 첫 반례와 정상 대조 | 완료 조건·남는 범위 |
|---|---|---|---|
| 1 | CJ-09/10, FS-01 / 1.1.05 | 활성 event의 범위 밖 FuncID·미등록 callback; 앞 RAM 적용 뒤 후반 section 실패; 정상 상태·undo | event 입력 거부와 전체 로드 rollback을 별도로 검증한다. rollback 자체 실패 시 실행을 재개하지 않는다. |
| 2 | FS-08/04/02 / 1.1.07 | 출력 크기 미기록 Zstd·잘린 frame; 손상 TOML로 Load→Save; 정상 사용자 지정 설정 | 실제 출력 길이·frame 완료·오류 반환·원본 보존. ROM 교체/reset 전에 실패를 반환한다. |
| 3 | FS-03/05/06/13 / 1.1.04 | A 저장 pending 중 실패하는 B 교체; flush 중 SetPath/버퍼 교체; 종료까지 쓰기 거부 | 경로·버퍼·버전의 소유권 일치, 다른 게임에 잘못 저장하지 않음, 미저장 결과·복구 선택 전달. |
| 4 | AD-01/02, FS-09/10 / 1.1.08·06 | mic upsample 끝 sample·생산/소비 count; controller→joystick; touch cancel/focus | 메모리 경계·동기화·닫힌 handle·입력 snapshot 검증. 실제 장치 수락은 합성과 구분한다. |
| 5 | NP-01/05~07/10~12 / 1.1.12·11·10 | 짧은 LAN payload·AID0; PCap caplen<len·잘린 DNS; GDB M/X 길이·escape | 잘못된 입력에서 부분 쓰기·과잉 복사 없음. 정상 프로토콜·재접속·연결 종료 보존. |

첫 증분은 CJ-09의 이벤트 값 검증과 FS-08/FS-04의 파일 경계·설정 보존을 1.1.03에 선반영한다. CJ-10/FS-01 전체 rollback, FS-02 전체 읽기·reset 경계, 설정 UI의 상세 복구 흐름은 각각 원래 과제에서 계속 관리한다.

1.1.04 증분은 CJ-10/FS-01의 load·undo 복원과 FS-02의 상태 파일 읽기 경계, FS-05 저장 잠금을 다룬다. [구현·검증 범위](releases/1.1.04.md)를 확인한 뒤 FS-03 ROM 교체의 저장 소유권과 FS-06 종료까지 지속되는 쓰기 실패 처리를 이어간다. 카트·DSi·host queue를 포함한 전체 상태 수락은 1.1.05 책임 범위에 남긴다.

[1.1.05 증분](releases/1.1.05.md)은 FS-03 교체 소유권, FS-06 종료 복구와 FS-02 카트 SRAM 읽기의 구현·로컬 회귀를 확인했다. [1.1.06 증분](releases/1.1.06.md)은 FS-02의 ROM 원시 읽기·save import 적용 순서, FS-07 archive 오류와 FS-14 RTC 파일 보호를 검증했다. 다음에는 입력·터치·마이크 묶음을 확인하고 네트워크 묶음을 이어간다. FS-13 이름 충돌은 기존 파일명 정책을 바꾸는 별도 작업으로 유지한다.

[1.1.07 증분](releases/1.1.07.md)은 FS-09/10의 장치·터치 수명과 AD-01/02의 마이크 입력 경계를 재현하고 로컬 회귀를 확인했다. 다음은 NP-01/05~07/10~12의 LAN·PCap/Slirp·GDB 입력 묶음이다. 실제 장치·플랫폼 수락을 기다리는 항목은 유지하며 독립적으로 재현 가능한 네트워크 경계를 먼저 다룬다.

[1.1.08 증분](releases/1.1.08.md)은 NP-01/05~07/10~12의 패킷 경계·메모리 요청·연결 종료를 수정하고 생성 입력과 실제 Windows loopback으로 확인한다. 빈 AID0 reply는 정상 구현으로 보존한다. 두 PC·실제 어댑터·DS DNS·ARM debugger 수락은 남긴다. 다음 증분은 NP-16/20/21의 homebrew·치트 파일/실행 입력을 실제 source와 대조하고, 잘린 입력·정상 입력 한 쌍부터 재현한다. FS-13 파일명 충돌, LAN handshake/LocalMP 수명과 다른 미착수 과제도 계속 유지한다.

[1.1.09 증분](releases/1.1.09.md)은 NP-16/20/21의 argv/DLDI와 치트 DB·literal 입력을 검증하고, 긴 치트 실행에 UI 메시지의 취소 요청을 연결한다. 정상 코드에 대한 임의의 작은 반복 상한은 추가하지 않는다. 다음은 FS-13 저장 파일명 충돌과 NP-13~15/17~19의 주변장치·파일 형식 후보를 소스·생성 입력으로 좁힌다. NP-21의 D1/D2 조건 복원과 C5 수명은 독립 의미 검증으로 유지하며 실기 수락을 추측으로 채우지 않는다.

[1.1.10 증분](releases/1.1.10.md)은 FS-13의 명시적 파일 선택과 활성 경로, NP-13 IR 위치·미지원 응답, NP-19 RTC edge·분 carry·Reset, NP-17 진동 AD1을 구현했다. 다음은 NP-18 EEPROM page wrap/dirty 통지와 AUXSPI CS 해제의 정상/실패 대조다. NP-14 BT 프로토콜과 NP-15 형식 판별은 근거를 확보한 부분만 좁혀 진행한다. 센서·RAM·실제 무선 통신과 모든 앞선 장치 수락은 그대로 남긴다.

[1.1.11 증분](releases/1.1.11.md)은 NP-15의 AUXSPI 부분 쓰기로 인한 CS 불일치와 NP-18의 작은 EEPROM page wrap/저장 통지를 재현·수정했다. 다음은 FS-15 치트 편집의 원자적 저장·오류 전달이다. 코어의 직렬화와 Qt 파일 commit을 분리하고 정상 roundtrip·쓰기 실패·편집 유지부터 검증한다. GBA EEPROM은 실제 DS ROM bus/DMA 소비자를 통한 read/write부터, BT는 command/IRQ/reply transport부터 독립 구현한다. 현재 부분 성공으로 이 기능들을 완료 처리하지 않는다.

[1.1.12 증분](releases/1.1.12.md)은 FS-15 편집 사본·직렬화 사전 검사·Qt 파일 commit과 실패 복구·실행 목록 갱신을 연결한다. 다음은 NP-21의 D1/D2 조건 복원과 C5 수명을 생산 실행기·정상 코드·명세와 대조한다. 결함을 가정해 게스트 의미를 먼저 바꾸지 않는다. LAN/LocalMP 수명·GBA EEPROM·BT와 앞선 실제 게임·장치 수락은 남은 독립 과제로 유지한다.

[1.1.13 증분](releases/1.1.13.md)은 NP-21의 D1/D2 매 반복 조건 복원·D0 기본 상태를 재현·수정하고 C5 원저자 자료를 대조한다. NitroHax의 영속 카운터가 Datel의 수명 증거는 아니므로 Datel 원 핸들러·실기 판정은 남긴다. 다음은 CJ-03 warmed ALU·shift·부분 flags와 CJ-06 빈 LDM/STM 목록의 CPU별 정의·실행 대조다. 현재 환경에서 독립 검증 가능한 core/JIT 경계를 진행하며 LAN·GBA EEPROM·BT와 장치 수락을 완료 처리하지 않는다.

[1.1.14 증분](releases/1.1.14.md)은 입력 설정의 실제 사용 문제를 우선 처리한다. SDL 장치가 있으면 Joystick 화면을 자동으로 여는 동작을 제거하고 Keyboard·Joystick 탭, 실제 dialog 저장·실행 적용·새 프로세스 재로드를 검증한다. 이어 CJ-03/CJ-06을 진행한다. 책임 버전 1.1.14의 JIT 과제는 미착수 상태이며 UI 수정으로 완료 처리하지 않는다.

[1.1.15 증분](releases/1.1.15.md)은 CJ-03의 실제 warmed ALU·shift·부분 flags를 보강하고 x64 ASR의 잘못된 호스트 피연산자와 Thumb 내부 사이클 누락을 재현·수정한다. 두 게임에서 일반 JIT/fastmem과 이전 코어 JIT의 최종 화면 digest는 일치한다. 인터프리터와의 기존 차이는 원인 미확정으로 유지한다. 다음은 CJ-06의 근거가 있는 ARM7 빈 목록 전송·PC·writeback부터 interpreter/decode/기존 fallback을 함께 연결한다. ARM9 호환 모델과 빈 PUSH/POP·DS 실기 타이밍을 ARM7 근거만으로 확정하지 않는다. CJ-03의 PC 복원·조건과 native A64도 남긴다.

[1.1.16 증분](releases/1.1.16.md)은 CJ-06의 ARM/Thumb LDM/STM 빈 목록을 CPU별로 처리하고, CJ-03의 일반 ARM7 STM PC값과 상태 전환 후 동일한 pipeline PC의 JIT 분기 누락을 수정한다. 빈 PUSH/POP의 원 실기 oracle은 확보하지 못했으므로 보류하며 기존 LR/PC 단독 경로와 구분한다. 다음은 CJ-05 DTCM remap 구간과 CJ-07 이벤트 취소의 실제 반례다. CJ-04 MPU·전체 CPU 명령·앞선 장치 수락도 유지하며, 원래 책임 버전과 실제 증분 릴리스 번호를 혼동하지 않는다.

[1.1.17 증분](releases/1.1.17.md)은 CJ-05의 DTCM 자체 view·이전 RAM 제외 구간·작은 bank offset과 CJ-07의 취소된 due event 실행을 수정한다. Windows 실제 view와 warmed guest, 재예약·snapshot 계약을 검증했다. 다음은 CJ-04의 MPU 권한 갱신과 exception 진입을 실제 guest의 정상/abort 한 쌍으로 좁힌다. CJ-08의 IRQ/DMA/sleep은 독립 이벤트 경계부터 확인하며, resize/save-load·다른 OS·native A64·장기 게임과 앞선 미완료 과제는 유지한다.

## 실제 증분 1.1.18 — 코어 경계와 오디오 진단 병행

CJ-04/CJ-08의 기존 착수 범위에 AD-04의 간헐적 틱 노이즈와 AD-08의 보간 품질 비교를 함께 배치한다. 보간을 켜고 출력 버퍼를 낮추면 드물게 발생한다는 사용 보고이며, 특정 보간 종류·버퍼 값·장치와 원인은 아직 확정하지 않았다. 아래 실제 증분 번호는 Release_Plan의 DSi 책임 버전 번호와 구분한다.

1. **재현·계측:** 같은 오디오 구간에서 보간 끔/켬과 낮은 버퍼/기본 버퍼를 대조한다. callback의 요구/공급 샘플 수, underrun·drop·대기와 출력 파형의 불연속을 계측한다. 평균 FPS나 CPU 사양만으로 원인을 판단하지 않는다.
2. **원인 수정:** 확인된 공급·동기화·무음 전환/재개 경계를 고친다. 기본 버퍼 확대나 저역통과 필터만으로 증상을 숨긴 것을 해결로 집계하지 않는다.
3. **보간 비교:** 기존 채널별 보간과 대역 제한 sinc 후보를 같은 입력에서 비교한다. 이미 있는 최종 출력 blip_buf 변환과 구분하고 음질·alias 억제·연산량·추가 지연을 평가한다. 개선 근거를 확보한 경우만 선택 옵션으로 통합한다.
4. **검증:** 기존 오디오 회귀와 재현 구간의 반복 실행으로 잡음·버퍼·지연을 대조한다. 생성 PCM 검사와 실제 장치 청취를 구분하고, 보간/버퍼 설정 저장·재시작 적용을 확인한다.
5. **출시 판정:** 입증한 수정과 비교 결과만 배포 기록에 반영한다. 드문 잡음의 미재현을 해결로 선언하지 않으며, sinc 채택 여부와 실제 장치·장기 실행의 남은 범위를 명시한다.

[1.1.18 구현 기록](releases/1.1.18.md)은 CJ-04의 warmed 정적 분기 권한 취소, AD-04의 생산량 기준 동기화와 부족/복귀 처리를 다룬다. AD-08 Q3는 고정 timer에서만 비교했고 제품 채택을 보류한다. 전체 MPU·장기 청취·물리 지연을 완료 처리하지 않는다.

## 실제 증분 1.1.19 — 코어 장치 이벤트·Compute 경계

CJ-08과 GR-02/03을 병행한다. 오디오 후보 확대에 머물지 않고 실제 코어·영상 소비자의 정확성과 비용을 계속 검증한다.

1. **근거·재현:** CJ-08은 IRQ/DMA/sleep 중 한 동시 경계를 생성 guest·장치 trace로 고른다. GR-02/03은 현재 Compute capture sampler와 capability/shader 실패 전달의 실제 반례·정상 대조를 준비한다.
2. **최소 수정:** 실행으로 확인된 예외 순서 또는 sampler/오류 전달만 수정한다. 예측한 결함이나 새로운 renderer 전체를 함께 넣지 않는다.
3. **통합:** 기존 guest 시간 단위·renderer fallback과 설정 저장 경로를 유지한다. 입력·오디오 등 공유 frontend 파일의 작성자는 하나로 둔다.
4. **검증:** 기존 core/GPU 회귀와 실제 OpenGL/Compute 출력·readback을 사용한다. 게스트 결과가 바뀌면 같은 게임·이전 코어와 대조하고 CPU 처리·GPU 대기를 구분한다.
5. **배포:** 입증한 범위만 다음 실행 파일과 공개 계획에 반영한다. 다른 OS/native A64·실기 타이밍·장기 플레이와 앞선 미완료 항목은 유지한다.

[1.1.19 구현 기록](releases/1.1.19.md)은 CJ-08의 Timer0→HALT→IRQ F 보존, GR-02의 capture sampler unit, GR-03의 capability 판정·실제 shader 실패·프런트엔드 복구를 검증한다. Windows 397개 검사와 두 게임의 10개 현재 최종 프레임은 통과했으며 같은 조건의 1.1.18 결과와 일치한다. 실제 GL3.2-only 장치·설정창 표시·전체 Qt event loop와 DMA/sleep·실기 타이밍은 남긴다.

## 실제 증분 1.1.20 — 캡처 보존·GL 자원 수명

GR-01/04/06을 진행한다. 같은 backend의 배율 변경 전후 데이터와 실제 source A/혼합 캡처 소비자를 확인한다.

1. **근거·재현:** GPU에만 남은 캡처→1x/2x 변경→CPU 읽기의 정상/실패 대조를 만든다. 별도 작업자는 실제 GL 생성·삭제를 계수해 정상 teardown과 첫 shader 실패의 자원 수명을 확인한다.
2. **최소 수정:** 재할당 전 필요한 캡처를 보존하고 확인된 누락 해제·미초기화 ID만 수정한다. 추정한 캡처 중간 타이밍·전체 RAII 재설계는 함께 넣지 않는다.
3. **통합:** source A/B·혼합 캡처의 metadata, 기존 texture cache·CPU/DMA 가시성·배율 설정 경계를 연결한다. 같은 파일과 공유 빌드는 한 작성자가 통합한다.
4. **검증:** 실제 두 GL backend·software와 CPU 읽기를 비교하고 재현된 크기·wrap·배율 조합을 검사한다. resource는 현재 context를 유지한 채 소유 객체의 소멸과 무관한 객체의 생존을 확인한다.
5. **배포:** 검증한 범위만 Windows 실행 파일·공개 기록에 반영한다. 중간 캡처 시각·실기·다른 드라이버/OS·native A64와 앞선 미완료 과제는 남긴다.

[1.1.20 구현 기록](releases/1.1.20.md)은 배율 변경의 캡처 유실·이미 읽은 캡처의 texture 참조, 반복 teardown의 program/buffer 누락과 초기 실패의 오삭제를 수정한다. source A/B·혼합·wrap·실제 guest LDRH·후속 texture 재사용을 검사했다. Windows 403개 검사와 두 게임의 세 renderer별 6개 현재 최종 프레임은 통과했으며 같은 조건의 1.1.19 프레임과 바이트가 일치한다. 중간 색 강도의 기존 1단계 차이는 GR-08 oracle 비교로 분리하고 DMA·캡처 진행 중 시각을 완료 처리하지 않는다.

## 실제 증분 1.1.21 — GL 한도·할당 실패와 context 경계

GR-07을 우선하고 GR-11/12의 실제 호출·소유 경계를 독립 조사한다. GR-05/08의 실기 시각·정밀도는 확인된 근거 범위에서만 진행한다.

1. **근거·재현:** 지원 배율의 크기 계산과 실제 GL 한도를 대조한다. 작은 정상 배율과 한도 초과/할당 실패 입력을 구성한다. context 생성·현재화·파괴가 어느 thread에서 가능한지 실제 호출부터 확인한다.
2. **최소 수정:** 입증한 실패에서 잘못된 자원 사용을 막고 기존 유효 출력 또는 software로 복구한다. GR-12 동시 생성이 도달 불가능하면 불필요한 global lock을 추가하지 않는다.
3. **통합:** 선택 배율·실제 활성 renderer·오류 전달·재선택의 상태를 맞춘다. 캡처 보존·부분 자원 해제·소유 thread 경계를 유지한다.
4. **검증:** 한도 판정/오류 주입과 실제 driver 할당 실패를 구분한다. 정상 픽셀·복구 뒤 프레임·재시도를 확인하고 변경한 경계의 기존 GPU/프런트엔드 회귀를 실행한다.
5. **배포:** 확인한 수정만 다음 Windows 실행 파일에 반영한다. 작은 VRAM 실장치·surface 상실/휴면/다중 창의 실제 화면·다른 OS/driver와 앞선 미완료 범위는 남긴다.

[1.1.21 구현 기록](releases/1.1.21.md)은 GR-07의 배율·장치 한도·할당 오류를 설정 실패로 전달하고 software 전환·캡처 보존·재선택을 검증한다. GR-11의 borrow 반환 신호 유실은 같은 mutex의 상태값으로 수정했다. 전체 Windows 417개 검사와 두 게임의 6개 현재 최종 프레임은 통과했으며 같은 조건의 1.1.20 프레임과 바이트가 일치한다. GR-12의 creator끼리 동시 진입 후보는 현재 GUI 경로에서 반증됐지만 GLAD 재로딩과 다른 worker 호출의 중첩은 후속으로 유지한다.

## 실제 증분 1.1.22 — 표시 context의 정지·재개·실패 전달

GR-11/12에서 확인한 실제 호출 경계를 진행한다. 창 교체/해제와 전역 GLAD 재로딩이 기존 worker의 GL 호출과 겹치는 조건을 우선한다.

1. **근거·재현:** deinit 응답 뒤 GUI panel 교체 전에 paused 표시를 진행시켜 재접근을 확인한다. 새 인스턴스 생성 중 기존 worker의 GL 호출·loader 쓰기를 독립 관찰한다. 화면용 shader/MakeCurrent 실패도 코어 shader와 구분한다.
2. **최소 수정:** panel/context 접근 정지를 GUI 교체·파괴가 끝날 때까지 유지한다. 초기화되지 않은 표시 경로와 실패한 current 상태로 진입하지 않도록 한다. creator끼리만 잠그는 것으로 worker 동시 읽기 문제를 해결했다고 하지 않는다.
3. **통합:** 검증한 borrow 반환 계약과 기존 메시지 순서를 재사용한다. 정지 중 worker init 응답을 기다리는 교착을 피하고 정상 재개·renderer/표시 fallback·오류 통지를 연결한다.
4. **검증:** 두 thread의 결정적인 순서와 실제 Windows context를 가능한 범위에서 대조한다. 표시 중단·정상 재개·반복 교체·새 인스턴스의 GL 호출 경계를 검사하고 게임 프레임·입력 상태 보존을 확인한다.
5. **배포:** 확인한 경로만 다음 Windows 실행 파일과 공개 기록에 반영한다. 물리 OOM·휴면/surface 상실·다중 GPU/DPI·다른 OS와 앞선 GPU 정밀도·오디오·실기 수락은 별도 gate로 유지한다.

[1.1.22 구현 기록](releases/1.1.22.md)은 해제 후 paused draw, root 코어 renderer/capture 정리와 GUI 창 변경 중 worker 접근을 다룬다. 새 context loader 경계와 borrow 중 worker broadcast도 함께 검증했다. 표시 shader/current/swap 실패의 복구는 독립 경계로 계속 진행하며 이번 성공으로 닫지 않는다.

## 실제 증분 1.1.23 — 표시 초기화 오류와 재시도

1. **재현:** 실제 ScreenShader/OSDShader compile 실패·MakeCurrent 실패·부분 생성 상태를 정상 초기화와 대조한다. OSD의 rendered 상태와 texture 소유 수명을 재초기화 전후로 확인한다.
2. **수정:** 표시 초기화의 성공 여부와 부분 해제를 연결한다. 실패한 current 상태에서 GL 호출을 계속하지 않고 성공한 초기화만 ready로 공개한다.
3. **통합:** worker→GUI 오류 통지와 기존 native 표시·설정·재선택을 연결한다. core renderer가 context보다 오래 살아남지 않도록 하며 borrowed worker의 응답을 기다리는 교착을 피한다.
4. **검증:** 정상·실패·재시도·반복 해제와 OSD가 있는 재초기화를 실제 Windows GL에서 검사한다. 가능한 전체 Qt 경로와 paused 게임 이미지·입력 유지도 별도로 확인한다.
5. **배포:** 입증한 복구만 공개 기록과 다음 실행 파일에 반영한다. 실제 surface/장치 상실·이종 GPU·다른 OS와 앞선 미완료 과제는 유지한다.

[1.1.23 구현 기록](releases/1.1.23.md)은 표시 shader·첫 MakeCurrent 실패, 부분 해제·OSD 재생성과 전체 Qt 두 창의 native 전환·명시적 재선택을 검증한다. 이미 실행 중인 core/표시 context의 실패는 다음 증분에 남긴다.

## 실제 증분 1.1.24 — 실행 중 current/swap 실패와 해제 거부

1. **재현:** 정상 GL 게임과 paused 표시에서 root/보조 창의 MakeCurrent·SwapBuffers 실패를 주입한다. core renderer가 남은 상태의 deinit·창 교체·종료도 대조한다.
2. **수정:** 확보하지 못한 context로 frame·shader·capture·삭제를 진행하지 않는다. 성공한 해제만 GUI의 context 파괴와 교체를 허용한다.
3. **통합:** worker→GUI 오류와 명시적 재시도를 연결한다. context 재확보 후 capture/core 객체를 정리해 native로 전환하며, 지속 실패 중 게임 상태와 기존 자원을 보존한다.
4. **검증:** 실제 GL과 전체 Qt에서 일시/지속 실패·재시도·창 닫기·보조 창을 확인한다. paused 게임 이미지·입력 유지도 검증하며 물리 surface 상실과 주입을 구분한다.
5. **배포:** 입증한 실패 복구만 반영한다. 이종 GPU/DPI·다른 OS·실기·장기 수락과 공개 전체 계획의 미완료 범위는 유지한다.

[1.1.24 구현 기록](releases/1.1.24.md)은 실행 중 표시 실패 전달·core 소비 차단·해제 실패 시 창 보존과 재시도를 다룬다. 전체 Qt의 생성 DS 실행에서 주 창·보조 창의 실패·재시도, 일시정지 화면과 프레임 보내기·입력 보존을 확인했다. 물리 surface 상실이나 일반 context 반환 실패까지 완료 처리하지 않는다.

## 실제 증분 1.1.25 — context 반환과 worker 소유권

1. **재현:** DoneCurrent 실패를 일반 worker borrow와 native context 생성 경계에서 정상 반환과 대조한다. 실패를 무시한 GUI 접근·loader 재진입이 실제 가능한지 확인한다.
2. **수정:** 반환하지 못한 context의 소유권을 다른 thread에 넘기지 않는다. 실패한 borrow를 성공으로 응답하지 않으며 기존 창·core 상태를 보존한다.
3. **통합:** 기존 실패 상태·GUI 재시도와 연결하고, 거부·중첩 borrow·종료에서 mutex와 응답 수를 일치시킨다. 반환 실패가 입증되지 않은 경로는 불필요하게 재설계하지 않는다.
4. **검증:** 결정적인 worker 순서와 실제 Windows context를 이용해 실패·재시도·정상 생성·해제를 확인한다. 도달 가능한 다중 인스턴스 경계를 별도로 검증한다.
5. **배포:** 재현·해결된 경계만 반영한다. 물리 장치·다른 OS·실기·장기 검증과 공개 전체 계획의 미완료 범위를 유지한다.

[1.1.25 구현 기록](releases/1.1.25.md)은 worker 반환 실패의 거부·부분/중첩 소유권 반환과 새 context의 GUI 정리를 다룬다. 전체 Qt에서 생성 DS 두 개·세 인스턴스·네 창의 실패·재시도·native 전환을 확인했고, 생성 fallback 설정 불일치와 종료 후 앱 알림 충돌도 수정했다. 물리 driver 오류와 다른 플랫폼까지 완료 처리하지 않는다.

## 실제 증분 1.1.26 — 독립 패치 묶음

| 블록 | 구현·소유 경계 | 확인한 결과 |
|---|---|---|
| DSi 부팅: AD-09/10/13 | DSi·NAND metadata·NDS direct boot·EmuInstance | 독립 암호 vector·정확한 읽기·사전 실패의 세션 보존·늦은 실패의 정지 |
| SD 전송: AD-11/12 | DSi_SD·FATStorage | 길이/sector 경계·backing 실패·FIFO/IRQ·명시적 복구 |
| LocalMP: NP-03 | LocalMP·전용 큐 fixture | 느린 수신자 넘침·record/permit 정합·종료/새 host |
| 그래픽 설정·통합: GR-03 | OpenGLSupport·Compute·EmuThread·VideoSettingsDialog·공유 CMake | 선택/실제 renderer·worker capability·실패/Cancel/재시도와 부팅 실패의 실행 차단 |

[1.1.26 구현 기록](releases/1.1.26.md)에 반례·5페이즈·최종 빌드와 498/498 회귀·전체 Qt의 확인 범위를 기록했다. 세 구현 블록은 격리된 작업 공간에서 진행했고 공유 연결부는 순차 통합했다. 새 제품 소스 파일은 없다. 실제 게임·물리 장치·장기 수락과 전체 계획의 미착수 과제는 유지한다.

## 실제 증분 1.1.27 — 렌더 대기·NDMA 정지·Wi-Fi 응답

| 블록 | 구현·소유 경계 | 확인한 결과 |
|---|---|---|
| Software 3D: GR-13 | GPU3D_Soft·GPU reset·software thread fixture | 작업 시작 전 저장/재설정 경쟁·완료 이중 대기·abort/중단·재시작·실제 픽셀/상태 복원 |
| DSi NDMA: AD-16 | DSi GX stall·DSi_NDMA·기존 core device fixture | 포화 FIFO 뒤 명령·완료 IRQ 보존·경쟁 채널·정상 즉시/VBlank 전송·저장/복원 |
| DSi Wi-Fi: NP-08 | DSi_NWifi·SDIO/WMI fixture | 명령 거부·기존 연결/scan 보존·association 본문·IRQ/credit 순서·명시적 재연결 |

[1.1.27 구현 기록](releases/1.1.27.md)에 수정 전 반례와 최종 Windows 빌드·509/509 회귀·전체 Qt의 renderer 교체/재시도/종료를 기록했다. NDMA와 Wi-Fi는 격리된 작성자가 맡고 공유 연결부는 순차 통합했다. 새 제품 소스 파일은 없다. AD-15의 장치 완료 타이밍은 변경하지 않았으며 각 항목의 물리·실제 게임·장기 수락은 유지한다.

## 실제 증분 1.1.28 — 캡처 중간 접근·태그·재설정

| 블록 | 구현·소유 경계 | 확인한 결과 |
|---|---|---|
| 캡처: GR-05 | GPU_OpenGL·기존 GL/core capture fixture | 중간 CPU 읽기/쓰기·DMA·과거/미래 줄·source/목적지/크기 변경 |
| AES: AD-14 | DSi_AES·단일 real-core fixture | FIFO/레지스터 태그·짧은 tag/padding·독립 암호 vector·FIFO/NDMA·복원 |
| DSi reset/I2C: AD-17/22 | DSi·DSi_I2C·기존 CoreDeviceExecution | SCFG와 실제 클럭/카드 경로·방향/STOP/IRQ·이전 상태 호환 |

[1.1.28 구현 기록](releases/1.1.28.md)에 수정 전 반례와 최종 Windows 빌드·517/517 회귀·전체 Qt 검증 범위를 기록했다. 기존 GPU·DMA·I2C·저장 도구를 재사용하며 새 제품 소스 파일은 없다. 실제 게임·물리 타이밍·부팅·NWRAM 등 미확정 조건은 완료로 보지 않는다.

## 실제 증분 1.1.29 — LAN·입력 identity·타이틀 교체

| 블록 | 구현·소유 경계 | 확인한 결과 |
|---|---|---|
| LAN: NP-02/FS-17 | LAN·MPInterface·LANDialog와 공유 frontend 호출 | 빈 제어 packet·초기화/종료·참가자 목록·비동기 연결/취소·실제 loopback/Qt 재시도와 수명 |
| 입력: FS-12 | EmuInstanceInput·JoystickSelection·InputConfig·Config | 누락/재정렬/동일 모델·숫자 설정 이관·인스턴스 배정·Cancel·설정 재로드 |
| DSi title: FS-16 | DSi_NAND·TitleManagerDialog | 입력/TMD 확인·기존 save 보존·NAND staging/backup·rollback·복구/정리 실패 경고 |

[1.1.29 구현 기록](releases/1.1.29.md)의 최종 Windows 빌드와 548/548 회귀·기존 전체 Qt 생성 guest 검증은 통과했다. 실제 매체·두 PC 게임/다자간 mesh·물리 hotplug·다른 OS 수락과 공개 전체 계획은 유지한다. 고유 serial 없는 패드는 재연결 뒤 재선택하며, NAND transaction은 전원 상실 복구를 보장하지 않는다.

## 다음 독립 블록

1.1.30은 실제 다자간 LAN 반례, 성공 load/undo의 host 오디오 이력, source A→DMA-first/warmed JIT의 기존 검증 공백을 세 블록으로 작성했다. LAN v1 호환·지원 상대끼리의 포트 교환·늦은 peer 준비 통지, 성공 복원과 실패 복귀의 PCM 정책을 구현했고 캡처는 제품 코드 변경 없이 생성 장면 검증을 보완했다. 원본/구버전 설정 이관과 LAN 역할 교환의 성공·실패를 구분한다. 개별 결과와 최종 통합 판정은 [릴리스 기록](releases/1.1.30.md)을 따른다.

[1.1.31 증분](releases/1.1.31.md)은 기존 FAT 이미지의 마운트 실패를 포맷으로 처리하던 경로, BTDMP의 한-word 빈 큐 접근, Compute layer·variant 초기화를 다룬다. FAT은 실제 생성 이미지/인덱스/폴더 보존과 같은 이미지 재시도를, DSP는 checked STL 반례와 실제 I2S 호출 경로를 확인한다. 부족 오른쪽 0은 기존 Teakra의 안전정책으로 명시하며 실기 정확성으로 간주하지 않는다. Compute는 전후 생성 출력이 같았고 관찰된 화면 오류나 속도 향상으로 기록하지 않는다.

[1차 감사](Audit_2026-09-11.md)의 다음 후보는 malformed state의 section 경계, FAT export 중간 실패·ROM/GBA/firmware 길이, GDB·LocalMP의 수신 경계, GL alpha/capture 및 Compute workload 한도다. 먼저 호출 가능한 최소 반례를 만든 뒤 필요한 소유 파일만 수정한다. 동일 소스 현상은 기존 ID에 합치며, 실기 값이 없는 임의 zero-fill·작업 생략·사이클 변경을 정확한 기본값으로 채택하지 않는다. 후속 네트워크 보안 점검은 NP-01/02/03/07의 패킷 길이·송신자/peer·협상·수신 버퍼 경계를 포함하고 원본 v1 호환 대조를 유지한다.

[1.1.32 증분](releases/1.1.32.md)에서 상태 section, FAT/NAND export, LocalMP/GDB 입력의 세 제한 블록을 통합했다. Windows 607개 회귀, 실제 Qt 복구, 수정하지 않은 원본 14.0 생성 상태 이관이 통과했다. 파일별 보존을 폴더 전체 복구나 모든 상태·게임 호환으로 확대하지 않는다.

후속 항목은 아래 순서와 첫 산출물로 넘긴다. 하나의 제한된 수정·필요한 검증이 끝나면 다음 독립 블록을 시작하고, 외부 수락을 기다리는 항목은 근거와 재개 조건을 남긴다.

| 순서·소유 블록 | 기존 ID | 다음에 만들 반례·대조 | 완료 또는 재개 조건 |
|---|---|---|---|
| 1. 그래픽 | GR-06/07/08 | Compute 실제 producer의 workload 초과·indirect 한도, GL alpha와 texture 끝의 capture 검색 | 작업을 버리지 않는 결과 보존과 실제 backend 픽셀 대조; 한도 미도달은 반증으로 기록 |
| 2. JIT/ISA | CJ-06/16/18 | 기존 guest fixture를 이용한 빈 PUSH/POP·조건부 사이클·비정렬 메모리 비교 | interpreter/생성 코드 대조와 문헌 의미를 구분; ARM64 native·실기 타이밍은 해당 실행 환경에서 재개 |
| 3. 배포 결속 | BV-01/05/18 | 다른 source revision의 EXE를 현재 소스와 묶으려는 입력 | 빌드 시 source 신원과 package 입력을 연결해 불일치 거부; 동일 EXE·문서만 변경한 경우의 정책도 명시 |
| 4. 통신 안정성 | NP-01/02/03, FS-17 | timestamp 0~31 reply, host 큐의 시각 wrap·가변 대기·늦은 peer; 먼저 자동조절 없이 계측 | host ms/guest μs 분리, 정상/구버전 대조와 취소 응답성; 자동조절은 아래 계약과 관측 후 판정 |
| 5. 성능 기준선·컴파일러 | BV-08/09/10/12, CJ-18 | 동일 입력의 cold/warm compile·CPU 실행·GPU 전송/대기 비용, 학습 밖 workload | JIT/PGO/SIMD 후보를 병목별로 분리; LTO guard를 근거 없이 해제하지 않고 동일 결과·median/tail을 확인 |
| 6. 저장 후속 | FS-02/06, CJ-10 | export 충돌의 사용자 복구 경로·폴더 삭제 충돌·ROM/GBA/firmware 부가 길이 | 1.1.32 파일별 보존 유지, 읽기/동기화 추가 I/O 비용 계측; whole-directory·동시 writer·다른 파일시스템은 별도 범위 |

[추가 참고 자료](Reference_Notes_2026-09-11.md)는 Qt/Dolphin의 저장 오류 처리, ENet의 소유권, QEMU/Dynarmic과 원 논문의 차등 검증, Khronos/parallel-rdp의 한도·가시성·작업 분할을 위 ID에 연결한다. 자료 수집 완료와 코드 적용·반례 해결을 구별한다.

[최적화 조사](Optimization_Research_2026-09-11.md)는 계측·컴파일러·JIT·GPU·멀티코어·네트워크의 후보를 중복 ID에 합쳐 정리한다. 먼저 BV-10/CJ-18의 비용 분리와 BV-09의 대표 workload/holdout을 준비한다. 직접 block chaining·x64 cycle register·GPU upload 재사용은 해당 병목이 확인될 때 독립 블록으로 넘기며, 공유 CMake와 release 문서는 한 작성자가 통합한다.

[1.1.33](releases/1.1.33.md)은 Compute 실제 workload/indirect 초과의 순서 보존 분할, 클래식 GL alpha와 텍스처 끝의 캡처·dirty word 경계를 구현했다. 그래픽 후속에는 blending을 끈 Compute 반투명 출력 차이의 정상/실패 대조, 큰 장면의 분할 비용, native texture fallback의 품질·성능을 남긴다. 다음 개발 묶음은 이 작은 반례와 JIT/ISA·배포 source/EXE 결속을 독립 소유로 진행한다. 게임·실기·다른 OS 수락은 위 로컬 수정과 구분한다.

2026-09-12의 [1.1.34 구현](releases/1.1.34.md): Compute는 DISP3DCNT bit3을 무시해 합성을 끈 반투명 폴리곤·A5I3 texel을 배경과 섞었다. 실제 source-A 캡처→guest LDRH의 기존 32조합 중 16개가 실패했고, bit3 검사 후 합성 켜짐·꺼짐·배경 alpha0을 포함한 44조합이 통과했다. 클래식 GL은 alpha16에서 정수 기준 `0x81D0` 대신 `0x81F0`을 내던 8조합을 별도 정수 합성으로 수정했다. 기존 batch의 영역을 GPU의 별도 텍스처에 복사하고 6비트 RGB와 5비트 alpha로 합성한다. 세 렌더러 각각 alpha44조합과 겹침·낮은 밝기·bitmap 배경24조합이 같은 기대값을 통과했다. 새 GL 저장 공간의 할당 실패 시 캡처 보존·software 복구·재시도도 통과했다. 픽셀당 4바이트 추가 저장과 GPU 영역 복사 비용이 있어 성능 향상을 주장하지 않는다. JIT는 A64 생성 코드28/28과 x64 실제 cached dispatch를 포함한40/40을 통과했다. 낮은 modulate alpha와 fog는6비트 출력22조합 및 최종 캡처 소비자로 추가 검증했다. 배포 source/EXE 결속(BV-01/05/18), Windows 전체631/631과 실제 Qt 복구는 통과했다. 실제 게임·다른 장치 수락은 후속이다.

[1.1.35 구현](releases/1.1.35.md)은 위 JIT/ISA와 통신 후속을 진행한다. 빈 Thumb PUSH/POP은 ARM 문서상 UNPREDICTABLE이며 빈 LDM/STM의 동작을 추정해 이식하지 않는다. 실제 interpreter·cached x64·fastmem의40관측은 정확성 통과가 아니며, 정상 LR-only/PC-only60대조는 통과했다. 인위적 C=4의 ARM7 LR-only에서 interpreter13/JIT10사이클 차이도 관측했으므로, 그 메모리/파이프라인 조건의 근거를 먼저 확인한다. 비정렬 메모리와 native A64·실기 의미도 후속이다.

LAN은64비트 host 경과 시간과 남은 대기를 사용하고, 새 참가자의 첫 유효 응답에는 기존 비활동 대기를 갱신한다. 중복·다른 MP 트래픽으로 계속 연장하지 않으며64MP 처리 뒤 부분 결과를 반환한다. 정상적으로20ms·40ms에 나눠 응답한 참가자를 전체25ms 한도로 자르지 않는다. 변경하지 않은1.1.34 소스39/44와 수정본44/44 대조에서 정상 느린 응답은 양쪽 모두 통과했다. 다음 통신 산출물은 대기 사용량·새 응답/중복·빈 반환·큐 나이·처리 상한의 관찰 전용 계측과 제한된 지터/손실/재정렬 입력이다. 자동조절은 그 증거와 복귀 정책을 정한 뒤 적용한다.

[1.1.36 구현](releases/1.1.36.md)은 ARM7 LR-only 차이를 전체 guest 주소의 `DataRegion >> 4` 판별 오류로 좁혔다.24비트 영역 판별로 고쳐 기존 timing 식을 올바른 메모리 영역에 적용했고, 다섯 profile·두 정렬의 interpreter/JIT/fastmem과 실제 cached 코드로 검증했다. 무조건3사이클을 더한 수정이 아니며 다른 영역 대조에서는10→9로 줄어든다. 실기 절대 timing과 비정렬 메모리·native A64는 후속이다.

LAN은 수신/큐/만료/거부·실제 host 대기·새 응답/중복/부분 반환/상한을 관찰하는128바이트 통계와 기존 로비 표시를 추가했다. 대기 정책과 guest timing을 유지한다. 같은 생성 경로의 양수 대기 계측 CPU 비용은 중앙값 약58ns 증가했으며 실제 소켓·게임 FPS·UI 비용의 증거는 아니다. 다음 독립 블록은 제한된 지터/손실/재정렬과 큐 나이·빈 반환·느린 참가자 진행 대조다. 이어 cold/warm JIT·CPU/GPU 비용 기준선과 holdout, export 복구·ROM 길이 경계를 진행한다. 자동조절은 여전히 미구현이며 먼저 유효 표본·악화/초기화·기본값 복귀 계약을 검증한다.

### 설정 없이 안정적으로 연결하기

NP-01/02/03과 FS-17의 후속 목표는 인터넷·PC·참여자 상태가 달라도 기본 설정으로 안정적으로 연결되는 것이다. 모든 환경에서 최저 지연을 보장한다는 뜻은 아니다. 먼저 host 수신 대기·큐 적체·지터·끊김을 관찰하고 게스트 Wi-Fi의 시간·IRQ 의미와 구분한다. 자동조절은 측정값과 검증된 제한 범위가 생긴 항목부터 적용한다.

- 기존 v1 형식과 구버전의 연결 절차를 유지한다. peer 협조가 필요한 확장은 지원 확인 뒤 사용하며 미지원·협상 실패 시 기존 경로로 돌아간다.
- 관찰만 하는 단계로 시작한다. 나쁜 한 번의 표본이나 절전·프레임 정지·peer 합류/이탈에 과민하게 반응하지 않도록 유효 표본·변경률·히스테리시스·복귀 조건을 명시한다.
- 대기 시간이나 큐만 늘려 끊김을 숨기지 않는다. 지연 상한·메모리 상한·취소/종료 응답성을 함께 검증하며, 상태가 나빠지거나 관측이 부족하면 검증된 기본값으로 복귀한다.
- 한 PC의 빠름을 기준으로 상대를 탈락시키지 않는다. 느린 peer, 가변 RTT·손실·재정렬, 대칭/비대칭 연결, 구버전 혼합을 포함한 같은 입력에서 기본값보다 악화되는 조건을 찾는다.
- host transport 개선을 DS의 가상 무선 timing 변경으로 대체하지 않는다. 프레임·패킷·연산 생략이나 emulated clock 변경이 필요하면 별도의 동작 변경으로 판정한다. 실제 두 PC·실제 게임은 생성 패킷 검증 뒤에도 남는 수락 조건이다.

먼저 넘길 산출물은 현재 계측·조절 지점 목록과 제한된 네트워크 상태 재현이다. 새 프로토콜, NAT 우회·중계 서비스, 게임 내 무선 메뉴 생략은 이 요구만으로 자동 도입하지 않는다.

1.1.32에서 발견한 `timestamp - 32` unsigned 경계는 [1.1.33](releases/1.1.33.md)에서 LocalMP·LAN의 안전한 차 비교로 수정했다. 실제 큐·loopback의 timestamp 0, age 32/33·미래·빈 AID0 응답을 대조했다. 다음은 host 큐 시각 wrap과 관찰 전용 계측·늦은 peer·취소 응답성이다. guest 32μs 창을 host 자동 대기 정책으로 바꾸지 않는다.

1.1.34에서는 host 하위 32비트 시각 0~15와 wrap 직전 수신한 정상 LAN 큐 패킷이 폐기되는 반례도 확인했다. 실제 수신 stamp와 public 수신 API를 통해 일반/command16입력을 대조하고 unsigned 경과 시간 비교로 기존 16ms 상한을 유지했다. 기존 packet37개와 새 통합 case, 실제 ENet 루프백이 통과했다. control event 처리 중 `time < time_last`의 조기 반환·남은 대기 budget은 이 큐 수정과 별도이며, 취소 응답성과 관찰 전용 계측의 다음 입력으로 남긴다. Windows 최종 통합은631/631을 통과했으며 이 결과가 실제 두 PC·혼합 구버전 통신 수락을 대체하지 않는다.

DSP 부족 출력·IRQ·채널 정렬, 실제 두 PC/구버전 혼합 LAN, 물리 오디오, Android·native A64·다른 OS·장기 게임 수락은 계속 열린 상태다. 실기 또는 해당 환경이 없다는 이유로 위 로컬 반례들을 함께 미루지 않는다.

core/DSP·GPU·연결/저장 파일을 독립 소유로 묶고 공용 CMake·프런트엔드 load·release 연결은 한 담당자가 순차 통합한다. 정해진 파일/명령의 수집과 검증·확정된 구현은 경량 작업자에게, 설계·타이밍·원인 판정은 총괄 또는 고성능 작업자에게 맡긴다. 배정 모델과 중단 뒤 실제 재개 모델을 구분하며 배정만으로 코드 검토 완료를 세지 않는다. 실기·native A64·플랫폼 수락을 기다리는 동안 독립 로컬 작업을 계속한다.

## 작업 소유권

EmuInstance.cpp를 건드리는 상태 로드·ROM 교체·Zstd 변경은 한 작성자가 순차 통합한다. 코어 NDS.cpp와 Config.cpp의 좁은 수정은 독립적으로 진행할 수 있다.
마이크와 입력 장치, LAN과 PCap/Slirp와 GDB는 독립 반례·소유 파일로 나눈다. 같은 파일의 동시 변경이나 같은 결과를 중복 구현하지 않는다.

반례 해결 뒤 기존 관련 테스트를 실행한다. 에뮬레이션 결과가 바뀌는 경우에만 interpreter·warmed JIT·GPU·게임 비교를 해당 경계로 넓힌다.
실기 명령 의미·DSP 타이밍·플랫폼 지원처럼 독립 oracle이 필요한 항목은 미검증 상태를 유지한다. 그 대기를 이유로 재현된 파일 보존 수정을 지연하지 않는다.

2026-09-12, 1.1.37: LAN 로비에 가장 오래 기다린 수신 패킷의 나이를 표시하며 기존 통신·만료·설정 정책을 유지한다. 생성 패킷의 재정렬·누락/늦은 응답·프레임 정지 뒤 큐를 확인했고, 비정렬 메모리의 정의된 ARM/Thumb 동작을 interpreter·cached x64 JIT·fastmem으로 검증했다. Windows 통합 640/640(포트 충돌을 피한 638+2 분할 실행, 실패·건너뜀 0)이 통과했다. 원본 클라이언트는 기존 5초 단일 대기에서 실패하고 같은 총 제한의 짧은 대기 반복 대조에서 재전송 후 연결됐다. 이는 수정하지 않은 원본의 호환 통과가 아니며 첫 UDP 미수신 원인·실제 두 PC 게임은 후속이다. 포켓몬 블랙의 반사광 픽셀 차이는 사용자 캡처로 확인했으나 동일 저장 상태/원본 대조 전이므로 그래픽 수정에 포함하지 않는다.

2026-09-12, 1.1.38: CJ-18/BV-10의 첫 제한 장면을 계측하고 JIT의 임시 배열 7개를 최대 32명령의 고정 용량으로 바꿨다. 설정·추적 길이·hash 입력은 유지한다. GCC x64에서 동적 스택 검사 호출 7→0, 고정 스택 예약 2096바이트이며 작은 block 설정에서는 예약량이 늘 수 있다. 동일 상태 120프레임·9쌍의 비계측 대조는 첫 frame 중앙값 9.4626→9.3478ms, warm 중앙값 4.9723→4.9259ms였지만 범위가 겹치므로 FPS 개선으로 판정하지 않는다. 별도 phase 계측에서는 cold 후보가 느리게도 측정됐다. 이 장면의 hash 약 0.046ms와 native 생성 약 3.36ms는 timer 영향을 포함한 120frame 합계이며 reset/reuse·holdout은 후속이다. 기존 ROMSmoke에 내장 DS BIOS·state 재생·선택적 frame CSV를 통합했다. CSV는 RunFrame 경과 시간으로 오디오 전달·최종 GL 대기와 구별한다. 세 렌더러의 동일 저장 장면 출력은 각자의 이전 결과와 바이트 단위로 같았으며 입력 파일 보존·120행 CSV·상태 오류·DSi 조합 거절·CSV 열기 실패를 검증했다. Windows 전체 640/640(실패·건너뜀 0)이 통과했다. 반사광 점은 흰 texel 처리 후 마지막 AA에서 coverage 6/32로 아래 적색과 섞이는 과정까지 재현했으나, 실기 및 실제 원본 프로그램 대조는 남아 있어 그래픽 계산은 수정하지 않았다. 다른 OS/native A64·장기 게임·실제 두 PC 수락은 별도다.

2026-09-12, 1.1.39: FS-02/19의 생성 입력으로 firmware의 할당 상한보다 큰 read 요청·Qt 오류값 수용, BIOS의 짧은 read/크기 불일치 성공·64비트 길이 축소, firmware 쓰기 거절 handle 누수, NDS의 padding이 숨기는 실행 데이터 누락을 재현했다. 펌웨어 파일은 512KiB 초과를 읽기 전에 거절하고 read 1개 완료를 요구한다. 기존 상한 이내 padding과 정상 128/256/512KiB는 유지한다. DS/DSi BIOS도 정확한 크기와 전체 읽기 후에만 적용한다. NDS는 원래 길이로 실행 영역을 검사한 뒤 padding하므로 미사용 끝부분만 잘라낸 정상 ROM은 허용한다. 기존 GBA 경계와 정상/실패 파일·내장 BIOS를 포함한 red→green 및 Windows 전체 643/643(실패·건너뜀 0)이 통과했다. 블랙 3모드와 솔라토로보 시작 장면 출력·사용자 입력 파일 보존도 확인했다. 과거 DSi 부팅 증상의 원인으로 단정하지 않는다.

BV-08은 GCC16.2/MSYS2에서 진단 전용 LTO core/ROMSmoke 및 실제 Qt 프로그램 빌드에 성공했고, 생산 guard는 유지했다. 같은 JIT/SIMD 설정의 Software 블랙 상태 120frame·솔라토로보 시작 1800frame을 각각 5쌍, 순서를 번갈아 CPU0에 고정해 대조했다. 모든 최종 출력은 같았다. 첫 frame/이후 frame 중앙값/p95/p99의 반복 중앙값(ms)은 블랙 9.5510/5.0182/5.7359/6.0552→9.2155/4.9360/5.4597/5.9833, 솔라토로보 11.1279/1.9468/3.4269/3.7497→12.4641/1.9364/3.3250/3.6663이었다. 솔라토로보 첫 frame 범위도 10.9505~11.3377→12.2109~12.6235로 늘어 기본 활성화 근거로 삼지 않는다. RunFrame만의 시간이며 파일 로드·오디오 전달·최종 GPU 대기·장기 플레이 성능을 포함하지 않는다. PGO 학습/검증을 수행한 결과도 아니다. 다음은 이 cold 비용의 원인 분리와 더 긴 workload, LTO Qt 실행 복구·native 플랫폼 및 compiler 조합별 판정이다.

2026-09-12, 1.1.40: FS-02의 FAT 폴더 동기화에서 guest 삭제를 호스트로 반영하기 전에 기존 내용 hash·종류·새 하위 파일을 검사한다. 충돌은 보존하고, 삭제 실패는 오류로 반환하며 같은 이미지의 다음 동기화에서 재시도한다. 재개 시 pending 삭제를 호스트 import보다 먼저 처리한다. 실제 FatFs·Windows 파일로 내용 수정/새 파일/종류 교체/삭제 잠금/인덱스 저장 실패와 정상 삭제를 검증했다. 읽기 전용 파일의 정상 삭제 및 실패 시 속성 복원을 유지한다. 사용자가 선택한 루트 별칭은 허용하며, MinGW가 하위 링크를 일반 폴더로 보는 반례에는 Windows reparse 속성 확인을 추가했다. 관련 43개 검사(실패·건너뜀 0)가 통과했다. 폴더 전체 원자성·검사 후 동시 writer 경쟁·복구 UI는 후속이다. hash 없는 구형 인덱스의 모호한 pending 삭제는 임의 승인하지 않는다.

BV-08 진단에서 LTO의 첫 frame 비용은 JIT 비활성 상태에서도 관측됐고, phase 계측은 ARM9 실행을 주요 구간으로 지목했다. ARM.cpp만 native object로 대체한 LTO 진단은 솔라토로보 초기 2frame·3쌍에서 첫 frame 중앙값 12.4169→11.4040ms(일반 11.2056ms)였다. 이후 블랙 120frame·솔라토로보 1800frame을 일반/후보 각각 5쌍 비교해 출력과 입력 보존을 확인했다. 첫 frame 중앙값은 블랙 9.9401→9.9318ms, 솔라토로보 11.1064→11.3431ms지만 반복 변동이 커 속도 개선으로 판정하지 않는다. 제품 LTO guard/CPU 코드는 유지한다. 다음은 조건을 통제한 장기 workload와 전체 Qt 실행·compiler별 검증이다.

1.1.40 최종 Windows 빌드·전체 657/657(79.63초, 실패·건너뜀 0)·배포 PE 129개 누락 import 0·clean PATH 시작이 통과했다. 별도 Qt 사용자 조작·실기·다른 OS 검증으로 확대하지 않는다.

2026-09-12, 1.1.41: LAN의 긴 수신 대기를 최대 25ms씩 나눠 종료 요청을 확인한다. 전체 정상 대기와 새 참가자 응답의 기존 갱신 정책, v1 패킷 형식은 유지한다. 실제 loopback의 1000ms 설정에서 종료 대기는 baseline 1000.284/999.979ms에서 0.139/24.954ms로 줄었고, 80ms 정상 timeout·60ms 지연 수신·재접속·4인 mesh가 통과했다. 생성 패킷 검사 51/51과 수정하지 않은 원본 8bdb579f LAN host/current client의 명령·응답·확인 및 timestamp 일치도 통과했다. 이는 자동조절 구현이나 두 PC의 실제 게임 수락이 아니며, 이전 원본 client 단일 대기의 첫 UDP 미수신 원인은 여전히 미확정이다.

FS-06은 종료 전 DSi/DLDI SD 폴더 동기화 실패를 Retry/Cancel/복구 이미지 선택에 연결한다. 같은 저장소로 재시도하며, 복구는 64KiB씩 읽어 별도 이미지로 원자 저장하고 원본/인덱스 별칭을 거절한다. 복사 성공은 폴더 동기화 성공으로 처리하지 않는다. 실제 FatFs 이미지와 Qt 위젯에서 내용 보존·취소·자식 인스턴스·재시도·복구 실패/재로드를 검증했다. 위젯 fixture의 생산자/EmuInstance는 대역이므로 실제 앱 종료·물리 장치 수락과 폴더 전체 원자성·동시 writer는 후속이다.

1.1.41 최종 Windows 빌드·전체 CTest670/670(92.96초, 실패·건너뜀0)·배포 PE129개 누락 import0·clean PATH 시작 및 소스 신원 대조가 통과했다. 최초 통합 링크의 테스트용 경로 함수 누락은 PlatformHeadless의 기존 파일 조회 대역에 보완했다. 앞선 SD short-read seed 생성 실패는 무수정 단독 및 최종 전체에서 재현되지 않았으나 원인은 미확정으로 남긴다.

FS-06 후속: 1.1.41의 실제 Qt main/EmuInstance/EmuThread/MainWindow와 생성 ARM 반복 코드·FAT 입력으로 8개 앱 실행이 통과했다. DSi/DLDI getter, 모달120ms 동안 CPU/프레임 정지, Cancel 후 실행 재개, 재시도 종료·8MiB 복구 전체 일치/재로드, 첫 카드 복구 뒤 둘째 카드에서 취소해도 원래 충돌 유지까지 확인했다. 생산 수정은 없었다. Software 표시·단일 실제 인스턴스의 검증이며 게임 부팅/guest SD 드라이버 부하·다중 인스턴스와 폴더 전체 원자성은 남는다.

2026-09-12, 1.1.42: LAN 자동 대기는 모든 준비된 상대의 서로 다른 최신 ENet ACK 표본과 RTT/변동을 사용한다. 설정값을 하한·복귀값으로 유지하고 100ms 상한, 초당 최대10ms 변경과 5ms 완충 구간을 적용한다. 신규/기존 기본25ms는 자동, 기존 사용자 지정값은 고정으로 이관하며 설정 창에서 끄거나 변경할 수 있다. 0ms 즉시 조회와 100ms 이상 고정값은 유지한다. 관측 부족·상대 변경·오래 멈춘 guest 수신·신뢰성 있는 명령의 높은 손실·연속3회 실제 수신 만료에서는 복귀한다. ENet 손실 값은 MP 데이터그램 손실률이 아니며, 처리량 상한에 따른 부분 반환을 수신 만료로 세지 않는다. v1 패킷·guest timestamp·큐의16ms 만료·새 상대 응답의 기존 대기 갱신은 바꾸지 않는다. 실제 설정 창의 Cancel/Apply/재시작·고정 모드 복원4회와 생성 패킷 검사가 통과했다. 실제 두 PC 게임·다른 OS 수락은 후속이다.

실제 ENet/loopback relay의 고정0/20ms·비대칭5/35ms·지터·4회 응답 누락·재정렬/복제 6조건을 각각20회 대조했다. 첫 수신 성공은 1.1.41에서20/0/0/5/0/0, 현재에서20/20/20/20/16/20이었다. 모든 명령·수신 payload/timestamp와 소유 프로세스 종료를 확인했고 의도적4손실을 성공으로 세지 않았다. 처음 검사 도구의 응답 후 반복 blocking drain이 가짜 만료를 만들었으므로 raw nonblocking drain으로 고쳐 양쪽을 다시 대조했다. baseline의4조건에서 각각1개 늦은 응답을 최종 관측하지 못한 결과도 보존하며 원인을 확정하지 않는다. 빠른 연결에서도 추정 대기 상한은25ms보다 커질 수 있고, 누락 시 기다리는 비용이 늘 수 있다. 실제 수신은 도착 즉시 반환하며 이 실험을 게임 FPS나 인터넷 전체 성능으로 해석하지 않는다.

1.1.42 Windows 전체 빌드와 CTest670/670(포트 분리668+2, 72.14+10.63초, 실패·건너뜀0), 원본8bdb579f host/current client의 생성 교환, 배포PE129개 누락0·clean PATH 시작·소스 식별 대조가 통과했다. 다음은 BV-05의 실제 재현된 임의 사용자파일 ZIP 포함 문제다. 정확한 배포 파일 목록을 입력으로 사용하고 나머지를 제외·알리며, 기존 소스 식별 검사를 유지한다. 이번 새 배포 폴더는 이전 검증 ZIP의365개 불변 파일과 새 EXE/README/launcher만 있음을 별도 대조했다. 장기 workload·실제 두 PC 게임·실기 AA·native A64/다른 OS와 기존 미완료 항목은 유지한다.

2026-09-12, 1.1.43: BV-05의 기존 packager는 알려진 확장자 외 임의 txt/json/jpg 등 생성5파일을 그대로 포함하면서 private_files_in_zip=0으로 보고했다. 이제 신뢰한 배포 입력에서 만든 필수 --runtime-manifest의 정확한 상대 경로/SHA-256만 압축한다. 추가 파일은 내용 해싱 없이 제외·개수 경고하고, 지정 파일의 누락/변조·경로 alias/외부 해석·잘못된 목록·runtime/입력 목록과 출력 충돌을 거절한다. 최종ZIP의 정확한 목록·내용 hash를 다시 검사하며 알려진 개인 파일 규칙의 개수와 전체 제외 개수를 구분한다. 기존 source identity 거절과 생성 runtime 전체 보존을 검증했다. Windows 대소문자 표기만 다른 유효 파일을 제외 개수에 중복 계산하던 후보 반례도11→10으로 수정했다. 이는 목록 생성자의 입력 신뢰를 자동 판정하거나 모든 동시 filesystem 변경을 원자화하는 기능은 아니다.

BV-02는 버전 정책을 한 CMake 파일에 모아 표시의 명시적 선행0을 유지하되 Windows RC 숫자에서는 제거한다. 기존1.1.0191은 windres가 경고와 함께 numeric1.1.137.0으로 만들었고 수정 후1.1.191.0이다. 0/9/09/10/99/100/0191/65535의 실제PE8조건과65536의 WORD 범위 초과 거절을 확인했다. 처음 overflow 검사 실패는 CMake 오류 문구의 줄바꿈 비교 문제였으며 해당 조건만 바로잡아 검증했다. 다른 compiler·OS 및 전체 UI/파일 정렬 수락으로 확대하지 않는다.

1.1.43 통합: Windows 빌드와 버전 경계를 포함한 전체671개 중670개가 첫 실행에 통과했다. PackageIdentity의 생성 저장소에 Python3.14가 만든 __pycache__가 들어가 정상 입력을 dirty로 거절한 실패를 재현했고, 실제 저장소와 같은 캐시 제외 규칙을 fixture에 적용해 해당1개를 재검증했다(79.00초 전체+18.76초 해당검사, 남은 실패/skip0). 제품의 dirty/source identity 거절은 유지한다. 테스트 소스 변경에 맞춰 실행 파일 식별값을 갱신했으며 배포PE129개 누락0·clean PATH·실제 버전1.1.43/숫자1.1.43.0·명시적368파일 목록과 소스 식별을 확인했다. 다음은 BV-03/04의 기존 배포 도구와 실제 의존성 복사 입력을 연결해 이 목록을 안전하게 생성하는 로컬 profile이다. 실제 두 PC/장기 게임·native A64/다른 OS·실기 AA 및 앞선 미완료 항목은 남는다.

2026-09-12, 1.1.44: BV-03/04의 기존 msys-dist.sh는 UCRT64 의존 경로를 선택하지 못해 DLL64종이 빠졌고 exit0 뒤 실행은0xC0000135였다. 새 로컬 배포 도구는 Qt의 명시적 파일 목록과 CMake의 전이 import를 사용하고, 복사 입력의 hash로 packager용 목록을 만든다. 기존 출력·누락/충돌·변조는 거절하며 버전이 붙은 EXE와 launcher 이름을 맞췄다. 설치 라이선스21개 누락 판정 중 ICU/Qt2개는 실제 설치 위치·동일 source/version으로 해소했고, 나머지19패키지의 공식 원문40개는 명시적 버전/hash 추가 입력으로 보존한다. 고지의 법적 완전성이나 전체 대응 소스 제공을 자동 인증하지 않는다.

Windows 전체672/672(52.89초, 실패/skip0), 실제 배포322파일의 입력/hash 대조·PE129개 누락0·clean PATH의 EXE/launcher 실행·배포 폴더 qwindows 로드·1.1.44 버전/소스 식별 일치를 확인했다. 검증 도구의 환경 변수 키와 Windows Qt 로그 출력 위치 문제는 도구에서 수정했으며 제품 실패로 집계하지 않는다. 다음은 BV-07의 toolchain/의존성 및 local patch 출처 대조다. 실제 두 PC/장기 게임·다른 OS/native A64·실기 AA와 앞선 미완료 수락은 유지한다.

2026-09-12, 1.1.45: BV-07의 libarchive 준비 도구는 두 번째 다운로드/해시 실패 전에 첫 기존 사용자 patch를 덮어썼다. 기본 실행을 검증·차이 보고로 바꾸고, 모든 pinned 파일·VCPKG license·release hash와 순차 patch 적용을 확인한 뒤 명시한 새 출력에만 후보를 만든다. 기존 출력은 거절하고 현재 overlay는 유지한다. 생성4그룹과 실제 공식8입력·3.8.9 소스의 patch 적용을 검증했으며 현재7파일과 생성 결과는 개행 차이 외 동일하다.

설치 도구5개·패키지90개·배포DLL128개의 hash/소유자·실제 링크 명령을 연결했다. 별도 probe가 실제 배포 DLL을 로드해 SDL2 2.32.10·libarchive3.8.9·zstd1.5.7·ENet1.3.18·Qt6.11.2·FAAD2 2.11.3을 설치 정보와 대조했다. FAAD 헤더의 unknown은 그대로 기록했고 첫 SDL_main 링크 실패는 probe의 진입점 선언을 수정했다. vendored7개는 포크 기준 tree/patch를 보존했으며 Teakra·libslirp2개에 변경이 있다. 이는 각 라이브러리의 원 upstream revision 인증이나 재현 빌드가 아니다. Windows 빌드·673/673(57.79초, 실패/skip0), 배포322파일·기존과 같은DLL128개·EXE/launcher/배포qwindows·버전/소스 식별이 통과했다. 다음은 BV-06의 실제 사용 중인 표준 기능과 지원 compiler/STL의 불일치를 확인한다. BV-07의 원 upstream revision/대응 소스와 장치·다른 OS·실기 수락은 후속으로 유지한다.

2026-09-12, 1.1.46: BV-06은 현재 GCC16.2/libstdc++에서 실제 사용처 compile과 bit/byteswap/span/stop_token/to_underlying/checked arithmetic 관련8개 실행이 통과해 제품 변경을 하지 않았다. GCC14·Clang/libc++·MSVC·다른 OS는 미검증이다. BV-13은 기존 RelWithDebInfo EXE를 보존하고 새 폴더에 GNU debug 정보와 실행 파일을 분리한다. debuglink CRC·양쪽 hash를 확인하며 기존 출력/정보 없는 입력/도구 실패는 성공 manifest 없이 거절한다. 기본적으로 입력 EXE를 실행하지 않고 명시 --build-info에서만 내장 메타데이터를 대조한다. 5개 행동 그룹과 실제 Qt 진단 빌드에서 실행 코드/시작 동등·NDS::RunFrame 주소의 NDS.cpp 줄 복원이 통과했다. 일반 Release 설정은 유지하며 실제 crash/ASLR/JIT unwind·다른 OS 수락으로 확대하지 않는다. Windows 전체674/674(58.08초, 실패/skip0).

GR-08의 새 편지 상태는 DSi·DSP HLE였다. 같은 입력의 3모드600프레임과 선택 커서가 생기는 추가 입력을 대조해 확대 마지막에 Software/Compute의 반사광 점이 고정되고 Classic에는 없는 것을 확인했다. 원인은 해당 위치에서 첫 흰 조각이 더 가까운 붉은 표면 아래에 기록된 뒤, 다음 흰 조각의 삽입으로 아래층 색이 바뀌어 마지막 AA에 빨강이 섞이는 경로까지 좁혔다. 추적270프레임은 기존 출력과 일치했다. 실기에서는 확대 중 점이 보이고 선택 화면에서 사라진다는 사용자 관찰/사진을 확보했으나 동기화된 디지털 픽셀 oracle은 없으며, 그래픽 제품 코드는 유지한다. 다음은 해당 깊이 보간/겹친 경계의 최소 실기 재현과 BV-07 원 upstream revision·대응 소스 연결이다.

1.1.46 배포 검증: 명시된322파일·PE129개 누락0, 이전 배포와 같은DLL128개 import 근거 재사용, clean PATH의 EXE/launcher 및 배포 qwindows 로드·버전/소스 식별 일치. 원본 사용자 자료와 기존 배포는 유지했다.

2026-09-12, 1.1.47: GR-10의 16비트 bitmap 변경 감시 범위가 실제 바이트 수의 절반이어서 뒤쪽을 바꿔도 GL 화면이 이전 내용을 유지했다. guest VRAMC와 flat cache는 정상인 반례를 확보하고 2바이트 단위를 반영했다. 128/256 크기·1x/2x·8비트 대조 및 네 면 홈브루의 첫 실행/재실행 출력이 일치한다. GR-08 홈브루는 게임과 같은 깊이 차이112/AA6/32 경로를 재현하며 Software/Compute의 AA OFF FFFF·ON94FD와 Classic FFFF·FFFF를 표시한다. 실기 값과 캐시 비용/상주량은 별도 미완료다. BV-07은 BUILD에 Teakra/libslirp 원본 revision을 연결했고 상속/fork patch6단계가 현재 tree와 일치한다. 이후 libslirp fuzz 입력31개의 잃어버린 symlink 의미 복원을 검토하며, 이는 runtime C 버전 갱신 근거가 아니다.

1.1.47 검증: Windows 빌드·전체677/677(57.67초, 실패/skip0), 실제 블랙 편지→선택600프레임의 세 모드별 이전 출력 일치와 개인 입력9개 hash 보존을 확인했다. 최종 core의 홈브루 첫 출력도 정상 재실행 기준과 일치한다. 배포322파일·PE129개 누락0, 같은DLL128개 import 근거 재사용, clean PATH의 EXE/launcher·배포 qwindows 및 버전/소스 식별이 통과했다. 실기 결과·다른 GPU/OS·장기 게임은 미완료다.

2026-09-12, 1.1.48: CJ-11 활성 예외 모드의 debugger SPSR 접근이 CPSR를 읽고 쓰던 결함을 수정했다. ARM9/7·5 bank·활성/비활성20조건은 interpreter/JIT에서 각각10/20→20/20이며 MRS와 MOVS 예외 복귀로 저장 상태의 실제 사용을 확인했다. p/P의 XML wire 번호와 내부 enum 불일치도 수정하고 완전한 길이·hex 검증 뒤에만 접근한다. 기존 XML/g/G 순서는 유지한다. 실제 TCP RSP의 XML 조회·p/P·g 교차 검증은 양 CPU에서 실패→통과했고 GNU ARM GDB UI 수락은 남는다.

BV-07은 원본 libslirp v4.8.0 alias31개를 실제 packet 사본/디렉터리로 저장해 소스 ZIP 소비 대상8/16→16/16, 전체19 corpus·61 packet 바이트 일치를 확인했다. source identity의 symlink 거절은 유지하며 향후 원본/사본을 함께 갱신해야 한다. POSIX 실행·fuzz campaign·스크립트 실행 bit 복원은 수행하지 않았다. Windows 전체 빌드와 CTest682/682(59.78초, 실패/skip0)가 통과했다. 다음은 실기 AA 값 판정과 CJ-12 DMA 타이밍의 실제 입력/기준 대조이며 native A64·다른 OS·장기 게임과 앞선 미완료 과제는 계속 남는다.

2026-09-12, 1.1.49: AD-03 생성자의 bitdepth 인자 덮어쓰기와 Auto 기종 조건 불일치를 기존 SetDegrade10Bit 경로 재사용으로 수정했다. 비공개 입력 없이 실제 DS/DSi 코어에 반복 PCM16을 공급하여 Auto/10/16비트의 최초 생성·재설정 출력을 setter 경로와 대조했고 3/6→6/6을 확인했다. Windows 전체 빌드·CTest682/682(79.95초, 실패/skip0)가 통과했다. 실제 오디오 장치와 청취 수락은 별도다. 구형 13.0 상태 가져오기는 GPU·scheduler·cart 구조와 과거 DSi 전원 의미의 변환 실험까지 진행했으며 제품에는 아직 넣지 않았다. 일반 저장 시점, DMA/SPI 진행 중 상태, 1.1로의 역방향 내보내기는 후속으로 남긴다.

2026-09-12, 1.1.50: CJ-01의 A64 곱셈 가변 I 누락·중복 fetch, signed early termination·long accumulate·Thumb의 원래 Rd 기준을 수정했다. [ARM7TDMI timing 표](https://documentation-service.arm.com/static/5e8e1323fd977155116a3129)와 실제 emitter 실행을 대조하면서 interpreter/x64의 추가 오류도 재현했다. Interpreter716조건은125실패→0, 캐시된 native x64는348실패→0, production A64를 Unicorn으로 실행한404조건도 최종0실패다. ARM7 multiply의 정의되지 않은 C는 oracle에서 제외하며, native A64 ABI/메모리 보호·실기 timing을 통과한 것으로 세지 않는다.

CJ-12는 기존 [DS 버스 실측](https://melonds.kuribo64.net/board/thread.php?pid=3805#3805)에 따라 GBA slot128KiB 마지막 halfword의 N 접근 비용을 반영한다. 실제 DMA 데이터·timestamp·IRQ136조건은80실패→0이며 mainRAM overlap·선점·ITCM 동시 실행은 남는다. CJ-16은 [VirtualFree 계약](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree)에 맞춰 동적32MiB JIT slice의 해제 인자를 수정했다. Windows x64의3회 누수가 모두 해제되고 native 코드18회 실행과 기본 슬롯 재사용이 유지됐다.

GR-09는 software2D의 효과별 scanline 분기·정수 RGB6 합성과 불필요한 두 번째 층 처리를 줄인다. 독립 scalar/baseline과18,550,784픽셀이 일치했고, 같은 최종 코어에 렌더러만 교체한 실제 블랙 편지→선택600프레임도 모두 일치했다. 생성5장면의 paired scanline wall 중앙값은3.02~6.82% 감소했다. 실제 게임600프레임의8쌍은 총 RunFrame 비용 변화가−17.70~+17.35%로 넓게 흔들려 전체 게임의 속도 향상은 확정하지 않았다. CJ-10은 카트 checksum/type/삽입 유무 불일치를 실제 로드 오류로 전파해 기존 rollback을 실행하며, 정상 로드·다음 frame 보존5조건을 검증했다. 상태 형식은14.2를 유지한다.

Windows 통합 빌드·전체 CTest693/693(93.02초, 실패/skip0)이 통과했다. 구형13.0 일반 가져오기·1.1 역방향 저장, 실기 AA·native A64·다른 OS·장기 게임 수락은 미완료다. 다음 독립 작업은 ARM9 cache/ITCM, x64 BMI, software3D·SPU 비용, libslirp 갱신이다. JIT 자기수정 코드의 추가 검증은 미완료다.

1.1.50 배포는322파일의 원본/hash·PE129개 누락 import0·기존과 같은DLL128개와 clean PATH의 EXE/launcher·배포 qwindows 로드·버전/소스 식별 일치를 확인했다. 개인 입력은 보존하며 패키지에 포함하지 않는다.

2026-09-12, 1.1.51: CJ-13은 ARM946E-S TRM 4.1.1에 따라 MPU를 끈 상태의 접근을 I/C enable bit와 무관하게 noncacheable로 처리한다. 실제 guest MCR·외부 fetch/load/store·MPU 전환·독립 TCM 대조60조건에서 interpreter/native x64 JIT/fastmem 각각26실패→0이다. 캐시 정책 변경을 가로지르는 기존 block·native A64·실기 절대 timing과 전체 cache 모델은 미완료다. GR-09는 software3D의 R/B 정수 합성을 묶어 곱셈6→4로 줄였다. 생성112프레임의 색/깊이/속성 및 같은 최종 core의 블랙600프레임이 기존 계산과 일치한다. 생성 AA 장면12쌍의 CPU cycle 중앙값8.13%, wall6.73% 감소는 해당 커널 조건의 결과이며 전체 게임 FPS 개선으로 확대하지 않는다.

BV-07은 출처를 고정한 libslirp4.9.4를 통합했다. 기존23개 공개 symbol·이전 header consumer·61개 corpus packet을 보존하고, 마지막 IPv4 fragment의 header 길이가 다른 실제 echo 누락을 수정한다. 최종 생산 Net_Slirp object와 vendored archive의 두 fragment 입력·runtime4.9.4를 확인했다. 호스트 UDP loopback의 첫 datagram 미수신은 이전/현재 및 독립 송수신에서도 남아 있어 해결로 세지 않는다. 실제 게임 Internet·다른 OS 수락은 후속이다. SPU pan과 BMI1 BIC 후보는 출력 검증과 별개로 성능 근거가 불충분하여 제품에 넣지 않았다.

Windows 통합 빌드·전체 CTest696/696(84.75초, 실패/skip0), 배포322파일·PE129개 누락 import0·기존과 같은DLL128개·clean PATH EXE/launcher/배포 qwindows·버전/소스 식별이 통과했다. 다음은 최대20개 독립 작업으로 오디오 전달/커널·입력/표시 지연·CPU/OS ISA dispatch·DSP·저장/플랫폼 호환을 분담하며, 원본1.1/현재 upstream과의 개선 및125개 계획의 구현·수락 상태를 별도로 대조한다. 개별 실행 검사 통과를 물리 지연·실기 일치·구형 상태 일반 호환이나 전체 계획 완료로 세지 않는다.


2026-09-12, 1.1.52: 20개 병렬 작업의 구현 후보15묶음을 통합했다. CJ-02 x64 조건부 PC 쓰기 cycle, AD-06 SPU channel capture, AD-20 DSP PDATA IRQ mask, 거절된 savestate header 재읽기, 압축 texture palette cache의 끝4바이트 누락을 수정했다. Software/Legacy/Compute 실제 guest capture에서 palette 교체가 반영된다. 구형13상태 일반 이관·실기 capture/IRQ timing·블랙 AA 문제 전체 해결을 의미하지 않는다.

오디오 mute 해제는 기존1ms recovery ramp를 사용하고 SSE2 PCM의0.5 경계 반올림을 맞췄다. 일시정지 중 제어 메시지 대기는 queue wake로 끊고, GL swap interval은 각 창을 current로 만든 뒤 적용한다. Qt의 오른쪽 modifier E0 scan code·GL header 기본 include·FMA4 OS 상태 판별도 바로잡았다. NEON RGB6 변환은 little-endian compiler baseline일 때만 자동 선택하며 기존 enum/낮은 ISA fallback을 보존한다. Compute clear 통합은 batch당 dispatch1회와 shader1개를 줄인다. payload 없는 archive의 전체 크기 선할당도 피한다. 실제 전체 FPS·입력/오디오/화면 물리 지연 향상은 아직 확정하지 않는다.

Windows 전체 빌드가 통과했다. 검사 프로세스 중단 전에 완료된635개 기록과 미완료64개의 후속 통과를 합쳤고, 기존 microbenchmark 옵션을 켜 반올림 verify1개만 추가했다. 현재 등록700개 모두 성공, 실패/skip0이며 성공한 full suite를 반복하지 않았다. 최종 core로 블랙 편지→선택600프레임을 세 renderer에서 실행했고 개인 입력/NAND/state hash를 보존했다. native ARM/macOS/Linux/Android, 물리 장치/장기 게임, 실제 두 PC 통신 및 기존 미완료 과제는 남는다.

125개 ID의1.1.51 고정 감사는 최소 구현·로컬 검증68, 좁은 계약 수락2, 부분28, 미착수22, 후보 보류/조건부/제외5다. 1.1.52의 AD-06/20 최소 증분2개와 FS-20 부분 착수를 반영하면 각각70/2/29/19/5다. 과제 크기가 다르고 실기/플랫폼 수락이 남아 있으므로 제품 완성도 백분율로 쓰지 않는다. 원본1.1과 현재 master의 상류 기능을 자체 개선으로 세지 않으며, 별도 릴리즈 문서가 없는37이후 기록도 이 실행 기록에서 이어진다.

다음은 보통2~4개의 필요한 독립 작업만 배정한다. Apple native SIMD/SME 호출 비용과 실제 처리 병목은 공식 자료·참고 프로젝트에서 조사하고, 출력이 NUL로 확정된 Windows GUI 로그의 불필요한 formatting 생략부터 작은 후보를 검증한다. 치트 구조 검증 재사용은 불변 실행본 소유권 확인 뒤 판단한다. C/C++ 표준 변경이나 assembly 사용 자체를 성능 개선으로 세지 않는다.

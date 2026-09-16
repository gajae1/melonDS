# 렌더링 캐시 정책 — 1.1.131

이 문서는 앱 소유 캐시와 GPU 드라이버의 캐시를 구분한다. 실기 정확성 인증이나 전체 프로세스 메모리 상한을 뜻하지 않는다.

## 저장 위치와 수명

| 종류 | 현재 정책 |
|---|---|
| Vulkan pipeline cache | VkDevice별 RAM 객체 하나. 파일로 내보내거나 이전 실행에서 읽지 않는다. |
| 캐시 재생성 | 같은 장치의 pipeline 생성에서만 재사용한다. 기기 간 데이터 공유나 드라이버 버전별 파일 디렉터리를 만들지 않는다. |
| OpenGL binary cache | 기존에는 호출되지 않는 파일 입출력과 주석 처리된 복구 코드만 남아 있었다. 1.1.131에서 제거했다. |
| 기존 shadercache 파일 | 이번 코드는 읽기/쓰기/삭제하지 않는다. 과거 다른 빌드의 파일 존재까지 부정하지 않는다. |
| 드라이버/OS 캐시 | 앱과 별도다. NVIDIA 등 드라이버가 자체 파일을 만들 수 있으며 이 기능은 전역 캐시를 삭제하지 않는다. |
| 텍스처·JIT·readback 버퍼 | compilation cache와 다른 자원이다. 이 버튼은 이들을 비우거나 게임 상태를 초기화하지 않는다. |

## 비우기

Video settings의 **Clear Vulkan pipeline cache** 버튼을 사용한다.
활성 Vulkan renderer가 있고 설정 변경/셰이더 준비가 대기 중이 아닐 때 활성화한다.
요청은 기존 EmuThread 메시지 큐로 전달한다. UI 스레드에서 Vulkan 캐시를 동시에 해제하지 않는다.
완료 전 중복 요청을 막고, renderer가 교체됐거나 없으면 성공으로 표시하지 않는다.
현재 VkPipeline과 화면·텍스처·guest capture·저장 데이터는 유지한다. 다음 pipeline 생성 때 캐시를 다시 준비한다.
다른 에뮬레이터 인스턴스와 다른 프로그램의 드라이버 캐시는 건드리지 않는다.

## 증가 감시

32개 pipeline 생성 묶음 뒤 vkGetPipelineCacheData의 크기 조회만 수행한다(pData=nullptr).
내보낼 수 있는 데이터 크기가 **32 MiB를 초과하면** 해당 RAM 캐시를 비운다. 큰 데이터 복사나 디스크 파일은 생성하지 않는다.
이 값은 직렬화 가능한 데이터 기준의 **소프트 제한**이다. 드라이버 내부 할당 총량, transient peak, 전체 RAM/VRAM의 엄밀한 상한이 아니다.
캐시 생성/크기 조회의 메모리 부족은 캐시 없이 동작하는 경로로 처리한다. 장치 상실 등 다른 오류는 감추지 않는다.

## 정확성 경계

캐시는 컴파일 준비 정보를 보관할 뿐 RGB6/alpha/depth/fog/AA 수식과 guest CPU/DMA 시점을 바꾸지 않는다.
실제 Vulkan 검사에서 캐시 사용/미사용, 수동 비우기, 크기 경계, 자동 비우기 이후의 전체 픽셀을 비교한다.
renderer 통합 검사는 texture format과 Z/W별 비우기 전후 현재 프레임을 비교한다.
이것으로 모든 드라이버·실제 게임·실기 화면의 오류 부재를 보장하지 않는다. 기존 native 그래픽 불일치는 별도 과제다.

## 외부 근거

- Vulkan pipeline/cache 수명: https://docs.vulkan.org/spec/latest/chapters/pipelines.html
- 데이터 크기 조회: https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html
- NVIDIA 드라이버의 별도 Shader Cache Size 설정: https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/nv3d/Manage_3D_Settings_%28reference%29.htm

실행 증거와 실제 수락 범위는 [1.1.131](releases/1.1.131.md) 및 [현재 상태](Current_Status.md)를 따른다.

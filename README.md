# DppBot

디스코드 음악봇. C++ 클라이언트([dpp](https://github.com/brainboxdotcc/DPP))와 Go 서버 두 프로세스로 구성된다.

- **클라이언트** (`src/`): 디스코드 명령어 처리, 길드별 음성 연결/재생 큐 관리, 서버 API 호출
- **서버** (`server/`): YouTube 검색/다운로드, PCM 변환, MySQL에 곡 정보 저장, 추천 목록 제공

두 프로세스는 로컬 HTTP(`127.0.0.1:8080`)로만 통신하며, 클라이언트는 `dpp::cluster::request()`(비동기 HTTP)로 서버를 호출한다 — 셸을 거치는 프로세스 실행은 쓰지 않는다.

## 상태

클라이언트/서버 초기 구현 완료.

## 구조

- [`server/`](server/) — Go 서버. 실행/API 사용법은 [server/README.md](server/README.md) 참고
- `src/` — C++ 클라이언트 (dpp)

## 요구사항

- **클라이언트**: C++17, CMake 3.20+, [dpp](https://github.com/brainboxdotcc/DPP), OpenSSL
- **서버**: Go 1.24+, MySQL, YouTube Data API v3 키, `yt-dlp`, `ffmpeg`

## 빌드

```bash
# vcpkg로 의존성 설치 후
cmake -B build-linux -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-linux
```

또는 `./auto-build.sh` (패키지 설치부터 빌드까지 한 번에), `./build.sh`, `./quick-build.sh`, `./clean-build.sh` 참고.

서버는 [server/README.md](server/README.md)의 빌드 방법을 따른다.

## 설정

클라이언트는 `config.json`(루트, `discord_token`), 서버는 `server/config.json`을 각각 필요로 한다.
`config.example.json` / `server/config.example.json`을 복사해 실제 값으로 채운다.

**이 `config.json` 파일들은 `.gitignore`로 커밋에서 제외된다 — 절대 저장소에 올리지 않는다.**

## 문서

- [DESIGN.md](DESIGN.md) — 아키텍처, 명령어, API 계약, 데이터 모델

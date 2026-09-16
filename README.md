# DppBot

디스코드 음악봇. C++ 클라이언트([dpp](https://github.com/brainboxdotcc/DPP))와 Go 서버 두 프로세스로 구성된다.

- **클라이언트** (`src/`): 디스코드 명령어 처리, 길드별 음성 연결/재생 큐 관리
- **서버** (`server/`): YouTube 검색/다운로드, PCM 변환, MySQL에 곡 정보 저장, 추천 목록 제공

두 프로세스는 로컬 HTTP(`127.0.0.1:8080`)로만 통신한다.

## 상태

설계 단계. 구현 방향은 [DESIGN.md](DESIGN.md)에 정리되어 있다.

## 요구사항

- C++17, [dpp](https://github.com/brainboxdotcc/DPP), OpenSSL, MySQL 클라이언트 라이브러리 (클라이언트)
- Go 1.24+, MySQL, YouTube Data API v3 키, `yt-dlp`, `ffmpeg` (서버)

## 설정

클라이언트와 서버 각각의 `config.json`에 비밀값(디스코드 토큰, YouTube API 키, DB 자격증명)을 둔다.
이 파일들은 `.gitignore`로 커밋에서 제외된다 — 절대 저장소에 올리지 않는다.

## 문서

- [DESIGN.md](DESIGN.md) — 아키텍처, 명령어, API 계약, 데이터 모델

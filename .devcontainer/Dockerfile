FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

USER 0
# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
    clang clang-format clang-tidy cmake g++ gdb git ninja-build \
    python3-pytest valgrind linux-tools-common linux-tools-generic \
    ca-certificates curl && \
    rm -rf /var/lib/apt/lists/* && \
    arch="$(dpkg --print-architecture)" && \
    case "$arch" in \
      amd64) hadolint_arch=x86_64 ;; \
      arm64) hadolint_arch=arm64 ;; \
      *) echo "Unsupported hadolint architecture: $arch" >&2; exit 1 ;; \
    esac && \
    curl -fsSL "https://github.com/hadolint/hadolint/releases/download/v2.12.0/hadolint-Linux-${hadolint_arch}" \
      -o /usr/local/bin/hadolint && \
    chmod +x /usr/local/bin/hadolint

# hadolint ignore=DL3066
USER vscode
WORKDIR /workspaces/CUSTOM-DB

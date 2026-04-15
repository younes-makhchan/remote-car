# syntax=docker/dockerfile:1

ARG PYTHON_VERSION="3.10.17"

# ==================================================================================
# Build Stage: Build and Install Python 3.10 from Source
# ==================================================================================
FROM container-registry.oracle.com/os/oraclelinux:9-slim AS build-python

ARG PYTHON_VERSION

RUN dnf -y update && \
    dnf -y install \
      gcc gcc-c++ make \
      openssl-devel bzip2-devel libffi-devel \
      zlib-devel sqlite-devel xz-devel \
      readline-devel tk-devel ncurses-devel \
      gdbm-devel uuid-devel wget curl tar \
    && dnf clean all

RUN set -eux; \
    PYTHON_DIR_VERSION="$(echo "${PYTHON_VERSION}" | awk -F. '{print $1"."$2}')"; \
    mkdir -p "/opt/python-${PYTHON_DIR_VERSION}"; \
    cd /tmp; \
    curl -fsSL "https://www.python.org/ftp/python/${PYTHON_VERSION}/Python-${PYTHON_VERSION}.tgz" -o "Python-${PYTHON_VERSION}.tgz"; \
    tar xzf "Python-${PYTHON_VERSION}.tgz"; \
    cd "Python-${PYTHON_VERSION}"; \
    ./configure \
      --prefix="/opt/python-${PYTHON_DIR_VERSION}" \
      --enable-optimizations \
      --with-ensurepip=install; \
    make -j"$(nproc)"; \
    make install; \
    cd /; \
    rm -rf "/tmp/Python-${PYTHON_VERSION}" "/tmp/Python-${PYTHON_VERSION}.tgz"

# ==================================================================================
# Final Stage: Oracle Linux 9 Slim + Python + Data Packages
# ==================================================================================
FROM container-registry.oracle.com/os/oraclelinux:9-slim

ARG PYTHON_VERSION="3.10.17"

COPY --from=build-python /opt/ /opt/

ENV PATH="/opt/python-3.10/bin:${PATH}" \
    PYTHONUNBUFFERED=1 \
    PYTHONIOENCODING=utf-8 \
    PIP_NO_CACHE_DIR=1

RUN dnf -y update && \
    dnf -y install \
      ca-certificates \
      libffi \
      openssl-libs \
      zlib \
      bzip2-libs \
      xz-libs \
      sqlite-libs \
      readline \
      ncurses-libs \
      gdbm-libs \
      uuid-libs \
    && dnf clean all

RUN python3.10 -m pip install --upgrade pip setuptools wheel && \
    python3.10 -m pip install numpy pandas

WORKDIR /workspace

CMD ["python3.10"]
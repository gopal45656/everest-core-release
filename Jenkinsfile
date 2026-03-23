pipeline {
    agent any

    environment {
        WORKSPACE_DIR = "${WORKSPACE}"
        BUILD_DIR = "${WORKSPACE}/build"
        INSTALL_DIR = "${WORKSPACE}/build/dist"
        SDK_ENV = "/opt/ampliphy-xwayland/BSP-Yocto-Ampliphy-AM62x-PD23.2.1/environment-setup-aarch64-phytec-linux"
    }

    stages {

        stage('Checkout SCM') {
            steps {
                checkout scm
                sh '''
                    echo "Branch: ${GIT_BRANCH} | Commit: ${GIT_COMMIT}"
                '''
            }
        }

        stage('Clean') {
            steps {
                sh '''
                    echo "--------------------------------------"
                    echo "Clean Build Requested — removing build directory"
                    echo "--------------------------------------"
                    rm -rf "${BUILD_DIR}"
                '''
            }
        }

        stage('Cross Compile') {
            steps {
                sh '''
                    set -e

                    echo "======================================"
                    echo " Sourcing Yocto SDK Environment"
                    echo "======================================"

                    source "${SDK_ENV}"

                    # ✅ Fix PATH for EDM
                    export PATH=$HOME/.local/bin:$PATH

                    echo ""
                    echo "Checking EDM..."
                    if ! command -v edm >/dev/null 2>&1; then
                        echo "❌ EDM NOT FOUND"
                        exit 1
                    fi
                    which edm

                    echo ""
                    echo "Toolchain info:"
                    echo "  CC  = $CC"
                    echo "  CXX = $CXX"
                    echo "  LD  = $LD"
                    echo "  SYSROOT = $SDKTARGETSYSROOT"
                    echo ""

                    echo "--------------------------------------"
                    echo " Creating Build Directory"
                    echo "--------------------------------------"
                    mkdir -p "${BUILD_DIR}"
                    cd "${BUILD_DIR}"

                    echo "--------------------------------------"
                    echo " Running CMake configure"
                    echo "--------------------------------------"
                    cmake .. \
                        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
                        -DCMAKE_BUILD_TYPE=Release

                    echo ""
                    echo "--------------------------------------"
                    echo " Building PhyTest_EVSE"
                    echo "--------------------------------------"
                    make -j$(nproc)

                    echo ""
                    echo "--------------------------------------"
                    echo " Installing"
                    echo "--------------------------------------"
                    make install

                    echo ""
                    echo "--------------------------------------"
                    echo " Verifying Binary Architecture"
                    echo "--------------------------------------"
                    if [ -d "${INSTALL_DIR}" ]; then
                        find "${INSTALL_DIR}" -type f | xargs file 2>/dev/null || true
                    else
                        echo "⚠️ Install directory not found"
                        exit 1
                    fi
                '''
            }
        }

        stage('Archive Artifacts') {
            steps {
                sh '''
                    echo "Archiving artifacts from: ${INSTALL_DIR}"
                    ls -l ${INSTALL_DIR} || echo "No files found"
                '''
                archiveArtifacts artifacts: 'build/dist/**', allowEmptyArchive: false
            }
        }
    }

    post {
        success {
            echo "======================================"
            echo " PhyTest_EVSE Build SUCCESS"
            echo " Build : ${BUILD_NUMBER}"
            echo " Branch: ${GIT_BRANCH}"
            echo "======================================"
        }

        failure {
            echo "======================================"
            echo " PhyTest_EVSE Build FAILED"
            echo " Build : ${BUILD_NUMBER}"
            echo " Branch: ${GIT_BRANCH}"
            echo " Logs  : ${BUILD_URL}console"
            echo "======================================"
        }
    }
}

pipeline {
    agent any

    environment {
        WORKSPACE_DIR = "${WORKSPACE}"
        BUILD_DIR = "${WORKSPACE}/build"
        INSTALL_DIR = "${WORKSPACE}/build/dist"
        SDK_ENV = "/opt/ampliphy-xwayland/BSP-Yocto-Ampliphy-AM62x-PD23.2.1/environment-setup-aarch64-phytec-linux"
        TARGET_BOARD = "root@192.168.11.78"
        TARGET_DIR = "/root/PhyTest_EVSE"
        SSH_CREDENTIAL = "target-board-key"   // Jenkins SSH Credential ID
    }

    stages {

        stage('Checkout SCM') {
            steps {
                checkout scm
                sh 'echo "Branch: ${GIT_BRANCH} | Commit: ${GIT_COMMIT}"'
            }
        }

        stage('Build') {
            steps {
                sh '''
                    set -e

                    . "${SDK_ENV}"
                    export PATH=$HOME/.local/bin:$PATH

                    mkdir -p "${BUILD_DIR}"
                    cd "${BUILD_DIR}"

                    # Only run CMake if build dir is empty
                    if [ ! -f "Makefile" ]; then
                        cmake .. -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" -DCMAKE_BUILD_TYPE=Release
                    fi

                    # Build only if needed
                    make -j$(nproc)
                    make install
                '''
            }
        }

        stage('Archive Artifacts') {
            steps {
                sh 'echo "Archiving artifacts from: ${INSTALL_DIR}"'
                archiveArtifacts artifacts: 'build/dist/**', allowEmptyArchive: false
            }
        }

stage('Lint') {
            steps {
                sh '''
                echo "Running cppcheck..."
                cppcheck --enable=all --error-exitcode=1 .
                '''
            }
}
    post {
        success {
            echo "======================================"
            echo "Gopal Korrapati | PhyTest_EVSE Build SUCCESS"
            echo "Build : ${BUILD_NUMBER}"
            echo "Branch: ${GIT_BRANCH}"
            echo "======================================"
        }

        failure {
            echo "======================================"
            echo "You nasty Gopal korrapati | PhyTest_EVSE Build FAILED"
            echo "Build : ${BUILD_NUMBER}"
            echo "Branch: ${GIT_BRANCH}"
            echo "Logs  : ${BUILD_URL}console"
            echo "======================================"
        }
    }
}

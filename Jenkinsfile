pipeline {
    agent any

    options {
        // Wait 10 seconds after commit to aggregate multiple pushes
        quietPeriod(10)

        // Skip older builds if a newer commit comes in
        skipStagesAfterUnstable()
    }

    environment {
        WORKSPACE_DIR = "${WORKSPACE}"
        BUILD_DIR = "${WORKSPACE}/build"
        INSTALL_DIR = "${WORKSPACE}/build/dist"
        SDK_ENV = "/opt/ampliphy-xwayland/BSP-Yocto-Ampliphy-AM62x-PD23.2.1/environment-setup-aarch64-phytec-linux"
        TARGET_USER = "root"                // Replace with your target board username
        TARGET_IP = "192.168.11.78"         // Replace with your target board IP
        TARGET_DIR = "/home/root/PhyTest_EVSE" // Replace with target directory
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

        stage('Prepare Build Directory') {
            steps {
                sh '''
                    echo "Checking if build directory exists"
                    if [ ! -d "${BUILD_DIR}" ]; then
                        echo "Build directory not found. Creating..."
                        mkdir -p "${BUILD_DIR}"
                        cd "${BUILD_DIR}"
                        echo "Running CMake configure"
                        cmake .. \
                            -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
                            -DCMAKE_BUILD_TYPE=Release
                    else
                        echo "Build directory exists, skipping CMake configuration"
                    fi
                '''
            }
        }

        stage('Build & Install') {
            steps {
                sh '''
                    set -e
                    echo "Sourcing Yocto SDK Environment"
                    . "${SDK_ENV}"

                    export PATH=$HOME/.local/bin:$PATH
                    if ! command -v edm >/dev/null 2>&1; then
                        echo "❌ EDM NOT FOUND"
                        exit 1
                    fi
                    which edm

                    cd "${BUILD_DIR}"
                    echo "Running make (incremental if possible)"
                    make -j$(nproc) || exit 1
                    echo "Installing binaries"
                    make install || exit 1
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

        stage('Deploy to Target Board') {
            steps {
                sh '''
                    echo "Deploying binaries to target board by Gopal"
                    ssh ${TARGET_USER}@${TARGET_IP} "mkdir -p ${TARGET_DIR}"
                    scp -r "${INSTALL_DIR}/"* ${TARGET_USER}@${TARGET_IP}:${TARGET_DIR}/
                    echo "Deployment complete"
                '''
            }
        }
    }

    post {
        success {
            echo "PhyTest_EVSE Build SUCCESS | Build: ${BUILD_NUMBER} | Branch: ${GIT_BRANCH}"
        }
        failure {
            echo "PhyTest_EVSE Build FAILED | Build: ${BUILD_NUMBER} | Branch: ${GIT_BRANCH} | Logs: ${BUILD_URL}console"
        }
    }
}

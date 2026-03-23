pipeline {
    agent any

    parameters {
        booleanParam(
            name: 'CLEAN_BUILD',
            defaultValue: false,
            description: 'Delete build directory before compiling (full rebuild)'
        )
    }

    environment {
        BUILD_DIR  = "${WORKSPACE}/build"
        DIST_DIR   = "${WORKSPACE}/build/dist"
        SDK_ENV    = "/opt/ampliphy-xwayland/BSP-Yocto-Ampliphy-AM62x-PD23.2.1/environment-setup-aarch64-phytec-linux"
        PROJECT    = "PhyTest_EVSE"
    }

    stages {

        // ----------------------------------------------------------------
        // Stage 1: Checkout
        // ----------------------------------------------------------------
        stage('Checkout Source') {
            steps {
                checkout scm
                sh 'echo "Branch: ${GIT_BRANCH} | Commit: ${GIT_COMMIT}"'
            }
        }

        // ----------------------------------------------------------------
        // Stage 2: Optional clean
        // ----------------------------------------------------------------
        stage('Clean') {
            when {
                expression { return params.CLEAN_BUILD == true }
            }
            steps {
                sh '''
                    echo "--------------------------------------"
                    echo "Clean Build Requested — removing ${BUILD_DIR}"
                    echo "--------------------------------------"
                    rm -rf "${BUILD_DIR}"
                '''
            }
        }

        // ----------------------------------------------------------------
        // Stage 3: Cross-compile
        // ----------------------------------------------------------------
        stage('Cross Compile') {
            steps {
                sh '''
                    bash -c "
                    set -e

                    echo '======================================'
                    echo ' Sourcing Yocto SDK Environment'
                    echo '======================================'
                    source \\"${SDK_ENV}\\"
		    export PATH=$HOME/.local/bin:$PATH

		    echo "Checking EDM..."
		    which edm || { echo "EDM NOT FOUND"; exit 1; }

                    echo ''
                    echo 'Toolchain info:'
                    echo '  CC  = '\$CC
                    echo '  CXX = '\$CXX
                    echo '  LD  = '\$LD
                    echo '  SYSROOT = '\$SDKTARGETSYSROOT
                    echo ''

                    # ---- Create build directory -------------------------
                    mkdir -p \\"${BUILD_DIR}\\"
                    cd \\"${BUILD_DIR}\\"

                    # ---- Run CMake only when needed ---------------------
                    if [ ! -f CMakeCache.txt ] || [ ../CMakeLists.txt -nt CMakeCache.txt ]; then
                        echo '--------------------------------------'
                        echo ' Running CMake configure'
                        echo '--------------------------------------'
                        cmake .. \\\\
                            -DCMAKE_INSTALL_PREFIX=\\"${DIST_DIR}\\" \\\\
                            -DCMAKE_BUILD_TYPE=Release
                    else
                        echo 'CMake already configured — skipping'
                    fi

                    echo ''
                    echo '--------------------------------------'
                    echo ' Building ${PROJECT} (Incremental)'
                    echo '--------------------------------------'
                    make -j\$(nproc)

                    echo ''
                    echo '--------------------------------------'
                    echo ' Installing to \${DIST_DIR}'
                    echo '--------------------------------------'
                    make install

                    echo ''
                    echo '--------------------------------------'
                    echo ' Verifying Binary Architecture'
                    echo '--------------------------------------'
                    find \\"${DIST_DIR}\\" -type f | xargs file 2>/dev/null || true
                    "
                '''
            }
        }

        // ----------------------------------------------------------------
        // Stage 4: Archive artifacts
        // ----------------------------------------------------------------
        stage('Archive Artifacts') {
            steps {
                sh 'echo "Archiving artifacts from: ${DIST_DIR}"'
                archiveArtifacts(
                    artifacts: 'build/**',
                    fingerprint: true,
                    allowEmptyArchive: false
                )
            }
        }
    }

    // ----------------------------------------------------------------
    // Post actions
    // ----------------------------------------------------------------
    post {
        success {
            echo "======================================"
            echo " ${PROJECT} Cross Compilation Successful"
            echo " Build : ${env.BUILD_NUMBER}"
            echo " Branch: ${env.GIT_BRANCH}"
            echo "======================================"
        }
        failure {
            echo "======================================"
            echo " ${PROJECT} Build FAILED"
            echo " Build : ${env.BUILD_NUMBER}"
            echo " Branch: ${env.GIT_BRANCH}"
            echo " Logs  : ${env.BUILD_URL}console"
            echo "======================================"
        }
        always {
            cleanWs(
                cleanWhenSuccess: false,
                cleanWhenFailure: false,
                cleanWhenAborted: true
            )
        }
    }
}

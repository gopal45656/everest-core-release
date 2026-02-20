pipeline {
    agent any

    environment {
        BUILD_DIR = "build"
        NATIVE_BUILD_DIR = "build_native"
        SDK_ENV = "/var/lib/jenkins/tool_chain/environment-setup-aarch64-phytec-linux"
    }

    stages {

        // --------------------------------------------------
        // 1️⃣ Checkout Source
        // --------------------------------------------------
        stage('Checkout Source') {
            steps {
                checkout scm
            }
        }

        // --------------------------------------------------
        // 2️⃣ Static Analysis - cppcheck
        // --------------------------------------------------
        stage('Static Analysis - cppcheck') {
            steps {
                sh '''
                    echo "--------------------------------------"
                    echo "Running cppcheck"
                    echo "--------------------------------------"

                    cppcheck --enable=all \
                             --inconclusive \
                             --std=c++17 \
                             --language=c++ \
                             --suppress=missingIncludeSystem \
                             --error-exitcode=1 \
                             . 2> cppcheck-report.txt
                '''
            }
        }

        // --------------------------------------------------
        // 3️⃣ MISRA Compliance Check
        // --------------------------------------------------
        stage('MISRA Compliance Check') {
            steps {
                sh '''
                    echo "--------------------------------------"
                    echo "Running MISRA Check"
                    echo "--------------------------------------"

                    cppcheck --enable=all \
                             --addon=misra \
                             --std=c++17 \
                             --suppress=missingIncludeSystem \
                             --error-exitcode=1 \
                             . 2> misra-report.txt
                '''
            }
        }

        // --------------------------------------------------
        // 4️⃣ Native Build with Address Sanitizer
        // --------------------------------------------------
        stage('Native Build (ASan Enabled)') {
            steps {
                sh '''
                    set -e
                    rm -rf $NATIVE_BUILD_DIR
                    mkdir -p $NATIVE_BUILD_DIR
                    cd $NATIVE_BUILD_DIR

                    cmake -DCMAKE_BUILD_TYPE=Debug \
                          -DCMAKE_CXX_FLAGS="-fsanitize=address -g -O1 -Wall -Wextra -Werror" \
                          -DCMAKE_C_FLAGS="-fsanitize=address -g -O1 -Wall -Wextra -Werror" \
                          ..

                    make -j$(nproc)
                '''
            }
        }

        // --------------------------------------------------
        // 5️⃣ Run Unit Tests (Detect segfaults/leaks)
        // --------------------------------------------------
        stage('Run Unit Tests') {
            steps {
                sh '''
                    set -e
                    cd $NATIVE_BUILD_DIR

                    echo "Running tests..."
                    ctest --output-on-failure
                '''
            }
        }

        // --------------------------------------------------
        // 6️⃣ Cross Compile for ARM (Yocto SDK)
        // --------------------------------------------------
        stage('Cross Compile (ARM Yocto)') {
            steps {
                sh '''
                    bash -c "
                    set -e

                    echo '--------------------------------------'
                    echo 'Sourcing Yocto SDK Environment'
                    echo '--------------------------------------'
                    source $SDK_ENV

                    echo 'Compiler being used:'
                    echo \$CXX

                    if [ ! -d $BUILD_DIR ]; then
                        mkdir -p $BUILD_DIR
                    fi

                    cd $BUILD_DIR

                    if [ ! -f Makefile ] || [ ../CMakeLists.txt -nt Makefile ]; then
                        cmake ..
                    fi

                    make -j$(nproc)
                    make install

                    echo 'Verifying binary architecture'
                    file *
                    "
                '''
            }
        }

        // --------------------------------------------------
        // 7️⃣ Archive Artifacts
        // --------------------------------------------------
        stage('Archive Artifacts') {
            steps {
                archiveArtifacts artifacts: 'build/dist/**, cppcheck-report.txt, misra-report.txt', fingerprint: true
            }
        }
    }

    post {
        success {
            echo '======================================'
            echo 'CI Pipeline Successful ✅'
            echo 'Code is Safe, MISRA Checked, Leak Checked'
            echo '======================================'
        }
        failure {
            echo '======================================'
            echo 'CI Pipeline Failed ❌'
            echo 'Fix issues before merging!'
            echo '======================================'
        }
    }
}

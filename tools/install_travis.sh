#!/usr/bin/env bash

# Exit on error
set -e
# Echo each command
set -x

export PATH="$deps_dir/bin:$PATH"

# This variable will contain something if this is a release build, otherwise it will be empty.
export PIRANHA_RELEASE_VERSION=`echo "${TRAVIS_TAG}"|grep -E 'v[0-9]+\.[0-9]+.*'|cut -c 2-`

# In a release build, do something only if the BUILD_TYPE is "Release" as well.
if [[ "${PIRANHA_RELEASE_VERSION}" != "" && "${BUILD_TYPE}" != "Release" ]]; then
    echo "Release build detected, skipping non-release jobs."
    exit 0;
fi

if [[ "${BUILD_TYPE}" == "Debug" ]]; then
    if [[ "${PIRANHA_COMPILER}" == "gcc" ]]; then
        cmake -DPIRANHA_WITH_MSGPACK=yes -DPIRANHA_WITH_BZIP2=yes -DPIRANHA_WITH_ZLIB=yes -DCMAKE_PREFIX_PATH=$deps_dir -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=yes -DCMAKE_CXX_FLAGS="-fsanitize=address -Os" -DCMAKE_CXX_FLAGS_DEBUG=-g0 -DPIRANHA_TEST_NSPLIT=${TEST_NSPLIT} -DPIRANHA_TEST_SPLIT_NUM=${SPLIT_TEST_NUM} ../;
        make VERBOSE=1;
        ctest -E "thread|memory" -V;
    elif [[ "${PIRANHA_COMPILER}" == "clang" ]]; then
        cmake -DPIRANHA_WITH_MSGPACK=yes -DPIRANHA_WITH_BZIP2=yes -DPIRANHA_WITH_ZLIB=yes -DCMAKE_PREFIX_PATH=$deps_dir -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=yes -DPIRANHA_TEST_NSPLIT=${TEST_NSPLIT} -DPIRANHA_TEST_SPLIT_NUM=${SPLIT_TEST_NUM} ../;
        make VERBOSE=1;
        ctest -E "thread" -V;
    fi
elif [[ "${BUILD_TYPE}" == "Coverage" ]]; then
        cmake -DPIRANHA_WITH_MSGPACK=yes -DPIRANHA_WITH_BZIP2=yes -DPIRANHA_WITH_ZLIB=yes -DCMAKE_PREFIX_PATH=$deps_dir -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=yes -DCMAKE_CXX_FLAGS="-Og --coverage" -DPIRANHA_TEST_NSPLIT=${TEST_NSPLIT} -DPIRANHA_TEST_SPLIT_NUM=${SPLIT_TEST_NUM} ../;
        make VERBOSE=1;
        ctest -E "thread" -V;
        bash <(curl -s https://codecov.io/bash) -x $GCOV_EXECUTABLE
elif [[ "${BUILD_TYPE}" == "Release" ]]; then
    cmake -DPIRANHA_WITH_MSGPACK=yes -DPIRANHA_WITH_BZIP2=yes -DPIRANHA_WITH_ZLIB=yes -DCMAKE_PREFIX_PATH=$deps_dir -DCMAKE_INSTALL_PREFIX=$deps_dir -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=yes ../;
    make install VERBOSE=1;

    # These test risk either timeout or they use too much ram.
    ctest -E "gastineau|pearce2_unpacked|s11n_perf" -V;

    # Check that all headers are really installed.
    # NOTE: this will have to be adapted in the cmake overhaul (fix src/ dir and check for config.hpp).
    for f in `find $deps_dir/include/piranha -iname '*.hpp'`; do basename $f; done|grep -v config.hpp|sort  > inst_list.txt
    for f in `find ../src -iname '*.hpp'`; do basename $f; done|grep -v config.hpp|sort > src_list.txt
    export INSTALL_DIFF=`diff -Nru inst_list.txt src_list.txt`
    if [[ "${INSTALL_DIFF}" != "" ]]; then
        echo "Not all headers were installed. The diff is:";
        echo "--------";
        echo "${INSTALL_DIFF}";
        echo "--------";
        echo "Aborting.";
        exit 1;
    fi

    # Do the release here.
    if [[ "${PIRANHA_RELEASE_VERSION}" != "" ]]; then
      echo "Creating new piranha release: ${PIRANHA_RELEASE_VERSION}"
      set +x
      curl -s --data '{"tag_name": "'"${TRAVIS_TAG}"'","name": "piranha-'"${PIRANHA_RELEASE_VERSION}"'","body": "Release of version '"${PIRANHA_RELEASE_VERSION}"'.","prerelease": true}' "https://api.github.com/repos/bluescarni/piranha/releases?access_token=${GH_RELEASE_TOKEN}" 2>&1 > /dev/null
      set -x
    fi
fi

if [[ "${BUILD_TYPE}" == "Python2" || "${BUILD_TYPE}" == "Python3" ]]; then
    cmake -DPIRANHA_WITH_MSGPACK=yes -DPIRANHA_WITH_BZIP2=yes -DPIRANHA_WITH_ZLIB=yes -DCMAKE_PREFIX_PATH=$deps_dir -DCMAKE_BUILD_TYPE=Debug -DBUILD_PYRANHA=yes -DCMAKE_CXX_FLAGS_DEBUG=-g0 -DCMAKE_CXX_FLAGS=-Os -DCMAKE_INSTALL_PREFIX=$deps_dir  ../;
    make install VERBOSE=1;
    python -c "import pyranha.test; pyranha.test.run_test_suite()";
fi

if [[ "${BUILD_TYPE}" == "Python2" && "${TRAVIS_OS_NAME}" != "osx" ]]; then
    cd ../doc/sphinx;
    export SPHINX_OUTPUT=`make html 2>&1 >/dev/null`;
    if [[ "${SPHINX_OUTPUT}" != "" ]]; then
        echo "Sphinx encountered some problem:";
        echo "${SPHINX_OUTPUT}";
        exit 1;
    fi
    echo "Sphinx ran successfully";
    if [[ "${TRAVIS_PULL_REQUEST}" != "false" ]]; then
        echo "Testing a pull request, the generated documentation will not be uploaded.";
        exit 0;
    fi
    if [[ "${TRAVIS_BRANCH}" != "master" ]]; then
        echo "Branch is not master, the generated documentation will not be uploaded.";
        exit 0;
    fi
    # Move out the resulting documentation.
    mv _build/html /home/travis/sphinx;
    # Checkout a new copy of the repo, for pushing to gh-pages.
    cd ../../../;
    git config --global push.default simple
    git config --global user.name "Travis CI"
    git config --global user.email "bluescarni@gmail.com"
    set +x
    git clone "https://${GH_TOKEN}@github.com/bluescarni/piranha.git" piranha_gh_pages -q
    set -x
    cd piranha_gh_pages
    git checkout -b gh-pages --track origin/gh-pages;
    git rm -fr sphinx;
    mv /home/travis/sphinx .;
    git add sphinx;
    # We assume here that a failure in commit means that there's nothing
    # to commit.
    git commit -m "Update Sphinx documentation, commit ${TRAVIS_COMMIT} [skip ci]." || exit 0
    PUSH_COUNTER=0
    until git push -q
    do
        git pull -q
        PUSH_COUNTER=$((PUSH_COUNTER + 1))
        if [ "$PUSH_COUNTER" -gt 3 ]; then
            echo "Push failed, aborting.";
            exit 1;
        fi
    done
fi

if [[ "${BUILD_TYPE}" == "Tutorial" ]]; then
    cmake -DPIRANHA_WITH_MSGPACK=yes -DPIRANHA_WITH_BZIP2=yes -DPIRANHA_WITH_ZLIB=yes -DCMAKE_PREFIX_PATH=$deps_dir -DCMAKE_BUILD_TYPE=Debug -DBUILD_TUTORIAL=yes ../;
    make VERBOSE=1;
    ctest -V;
elif [[ "${BUILD_TYPE}" == "Doxygen" ]]; then
    # Configure.
    cmake ../;
    # Now run it.
    cd ../doc/doxygen;
    export DOXYGEN_OUTPUT=`$deps_dir/bin/doxygen 2>&1 >/dev/null`;
    if [[ "${DOXYGEN_OUTPUT}" != "" ]]; then
        echo "Doxygen encountered some problem:";
        echo "${DOXYGEN_OUTPUT}";
        exit 1;
    fi
    echo "Doxygen ran successfully";
    if [[ "${TRAVIS_PULL_REQUEST}" != "false" ]]; then
        echo "Testing a pull request, the generated documentation will not be uploaded.";
        exit 0;
    fi
    if [[ "${TRAVIS_BRANCH}" != "master" ]]; then
        echo "Branch is not master, the generated documentation will not be uploaded.";
        exit 0;
    fi
    # Move out the resulting documentation.
    mv html $HOME/doxygen;
    # Checkout a new copy of the repo, for pushing to gh-pages.
    cd ../../../;
    git config --global push.default simple
    git config --global user.name "Travis CI"
    git config --global user.email "bluescarni@gmail.com"
    set +x
    git clone "https://${GH_TOKEN}@github.com/bluescarni/piranha.git" piranha_gh_pages -q
    set -x
    cd piranha_gh_pages
    git checkout -b gh-pages --track origin/gh-pages;
    git rm -fr doxygen;
    mv $HOME/doxygen .;
    git add doxygen;
    # We assume here that a failure in commit means that there's nothing
    # to commit.
    git commit -m "Update Doxygen documentation, commit ${TRAVIS_COMMIT} [skip ci]." || exit 0
    PUSH_COUNTER=0
    until git push -q
    do
        git pull -q
        PUSH_COUNTER=$((PUSH_COUNTER + 1))
        if [ "$PUSH_COUNTER" -gt 3 ]; then
            echo "Push failed, aborting.";
            exit 1;
        fi
    done
fi

set +e
set +x

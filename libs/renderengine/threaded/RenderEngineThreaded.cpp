/*
 * Copyright 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "RenderEngineThreaded.h"

#include <sched.h>
#include <chrono>
#include <future>

#include <android-base/stringprintf.h>
#include <private/gui/SyncFeatures.h>
#include <utils/Trace.h>

#include "gl/GLESRenderEngine.h"

using namespace std::chrono_literals;

namespace android {
namespace renderengine {
namespace threaded {

std::unique_ptr<RenderEngineThreaded> RenderEngineThreaded::create(CreateInstanceFactory factory,
                                                                   RenderEngineType type) {
    return std::make_unique<RenderEngineThreaded>(std::move(factory), type);
}

RenderEngineThreaded::RenderEngineThreaded(CreateInstanceFactory factory, RenderEngineType type)
      : RenderEngine(type) {
    ATRACE_CALL();

    std::lock_guard lockThread(mThreadMutex);
    mThread = std::thread(&RenderEngineThreaded::threadMain, this, factory);
}

RenderEngineThreaded::~RenderEngineThreaded() {
    {
        std::lock_guard lock(mThreadMutex);
        mRunning = false;
        mCondition.notify_one();
    }

    if (mThread.joinable()) {
        mThread.join();
    }
}

// NO_THREAD_SAFETY_ANALYSIS is because std::unique_lock presently lacks thread safety annotations.
void RenderEngineThreaded::threadMain(CreateInstanceFactory factory) NO_THREAD_SAFETY_ANALYSIS {
    ATRACE_CALL();

    struct sched_param param = {0};
    param.sched_priority = 2;
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        ALOGE("Couldn't set SCHED_FIFO");
    }

    mRenderEngine = factory();

    std::unique_lock<std::mutex> lock(mThreadMutex);
    pthread_setname_np(pthread_self(), mThreadName);

    {
        std::unique_lock<std::mutex> lock(mInitializedMutex);
        mIsInitialized = true;
    }
    mInitializedCondition.notify_all();

    while (mRunning) {
        if (!mFunctionCalls.empty()) {
            auto task = mFunctionCalls.front();
            mFunctionCalls.pop();
            task(*mRenderEngine);
        }
        mCondition.wait(lock, [this]() REQUIRES(mThreadMutex) {
            return !mRunning || !mFunctionCalls.empty();
        });
    }

    // we must release the RenderEngine on the thread that created it
    mRenderEngine.reset();
}

void RenderEngineThreaded::waitUntilInitialized() const {
    std::unique_lock<std::mutex> lock(mInitializedMutex);
    mInitializedCondition.wait(lock, [=] { return mIsInitialized; });
}

void RenderEngineThreaded::primeCache() {
    ATRACE_CALL();
    // This function is designed so it can run asynchronously, so we do not need to wait
    // for the futures.
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::primeCache");
            instance.primeCache();
        });
    }
    mCondition.notify_one();
}

void RenderEngineThreaded::dump(std::string& result) {
    std::promise<std::string> resultPromise;
    std::future<std::string> resultFuture = resultPromise.get_future();
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([&resultPromise, &result](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::dump");
            std::string localResult = result;
            instance.dump(localResult);
            resultPromise.set_value(std::move(localResult));
        });
    }
    mCondition.notify_one();
    // Note: This is an rvalue.
    result.assign(resultFuture.get());
}

void RenderEngineThreaded::genTextures(size_t count, uint32_t* names) {
    ATRACE_CALL();
    std::promise<void> resultPromise;
    std::future<void> resultFuture = resultPromise.get_future();
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([&resultPromise, count, names](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::genTextures");
            instance.genTextures(count, names);
            resultPromise.set_value();
        });
    }
    mCondition.notify_one();
    resultFuture.wait();
}

void RenderEngineThreaded::deleteTextures(size_t count, uint32_t const* names) {
    ATRACE_CALL();
    std::promise<void> resultPromise;
    std::future<void> resultFuture = resultPromise.get_future();
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([&resultPromise, count, &names](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::deleteTextures");
            instance.deleteTextures(count, names);
            resultPromise.set_value();
        });
    }
    mCondition.notify_one();
    resultFuture.wait();
}

void RenderEngineThreaded::mapExternalTextureBuffer(const sp<GraphicBuffer>& buffer,
                                                    bool isRenderable) {
    ATRACE_CALL();
    // This function is designed so it can run asynchronously, so we do not need to wait
    // for the futures.
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([=](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::mapExternalTextureBuffer");
            instance.mapExternalTextureBuffer(buffer, isRenderable);
        });
    }
    mCondition.notify_one();
}

void RenderEngineThreaded::unmapExternalTextureBuffer(const sp<GraphicBuffer>& buffer) {
    ATRACE_CALL();
    // This function is designed so it can run asynchronously, so we do not need to wait
    // for the futures.
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([=](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::unmapExternalTextureBuffer");
            instance.unmapExternalTextureBuffer(buffer);
        });
    }
    mCondition.notify_one();
}

size_t RenderEngineThreaded::getMaxTextureSize() const {
    waitUntilInitialized();
    return mRenderEngine->getMaxTextureSize();
}

size_t RenderEngineThreaded::getMaxViewportDims() const {
    waitUntilInitialized();
    return mRenderEngine->getMaxViewportDims();
}

bool RenderEngineThreaded::isProtected() const {
    waitUntilInitialized();
    // ensure that useProtectedContext is not currently being changed by some
    // other thread.
    std::lock_guard lock(mThreadMutex);
    return mRenderEngine->isProtected();
}

bool RenderEngineThreaded::supportsProtectedContent() const {
    waitUntilInitialized();
    return mRenderEngine->supportsProtectedContent();
}

bool RenderEngineThreaded::useProtectedContext(bool useProtectedContext) {
    std::promise<bool> resultPromise;
    std::future<bool> resultFuture = resultPromise.get_future();
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push(
                [&resultPromise, useProtectedContext](renderengine::RenderEngine& instance) {
                    ATRACE_NAME("REThreaded::useProtectedContext");
                    bool returnValue = instance.useProtectedContext(useProtectedContext);
                    resultPromise.set_value(returnValue);
                });
    }
    mCondition.notify_one();
    return resultFuture.get();
}

bool RenderEngineThreaded::cleanupPostRender(CleanupMode mode) {
    std::promise<bool> resultPromise;
    std::future<bool> resultFuture = resultPromise.get_future();
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([&resultPromise, mode](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::cleanupPostRender");
            bool returnValue = instance.cleanupPostRender(mode);
            resultPromise.set_value(returnValue);
        });
    }
    mCondition.notify_one();
    return resultFuture.get();
}

void RenderEngineThreaded::setViewportAndProjection(Rect viewPort, Rect sourceCrop) {
    std::promise<void> resultPromise;
    std::future<void> resultFuture = resultPromise.get_future();
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([&resultPromise, viewPort, sourceCrop](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::setViewportAndProjection");
            instance.setViewportAndProjection(viewPort, sourceCrop);
            resultPromise.set_value();
        });
    }
    mCondition.notify_one();
    resultFuture.wait();
}

status_t RenderEngineThreaded::drawLayers(const DisplaySettings& display,
                                          const std::vector<const LayerSettings*>& layers,
                                          const std::shared_ptr<ExternalTexture>& buffer,
                                          const bool useFramebufferCache,
                                          base::unique_fd&& bufferFence,
                                          base::unique_fd* drawFence) {
    ATRACE_CALL();
    std::promise<status_t> resultPromise;
    std::future<status_t> resultFuture = resultPromise.get_future();
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([&resultPromise, &display, &layers, &buffer, useFramebufferCache,
                             &bufferFence, &drawFence](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::drawLayers");
            status_t status = instance.drawLayers(display, layers, buffer, useFramebufferCache,
                                                  std::move(bufferFence), drawFence);
            resultPromise.set_value(status);
        });
    }
    mCondition.notify_one();
    return resultFuture.get();
}

void RenderEngineThreaded::cleanFramebufferCache() {
    ATRACE_CALL();
    // This function is designed so it can run asynchronously, so we do not need to wait
    // for the futures.
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::cleanFramebufferCache");
            instance.cleanFramebufferCache();
        });
    }
    mCondition.notify_one();
}

int RenderEngineThreaded::getContextPriority() {
    std::promise<int> resultPromise;
    std::future<int> resultFuture = resultPromise.get_future();
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([&resultPromise](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::getContextPriority");
            int priority = instance.getContextPriority();
            resultPromise.set_value(priority);
        });
    }
    mCondition.notify_one();
    return resultFuture.get();
}

bool RenderEngineThreaded::supportsBackgroundBlur() {
    waitUntilInitialized();
    return mRenderEngine->supportsBackgroundBlur();
}

void RenderEngineThreaded::onPrimaryDisplaySizeChanged(ui::Size size) {
    // This function is designed so it can run asynchronously, so we do not need to wait
    // for the futures.
    {
        std::lock_guard lock(mThreadMutex);
        mFunctionCalls.push([size](renderengine::RenderEngine& instance) {
            ATRACE_NAME("REThreaded::onPrimaryDisplaySizeChanged");
            instance.onPrimaryDisplaySizeChanged(size);
        });
    }
    mCondition.notify_one();
}

} // namespace threaded
} // namespace renderengine
} // namespace android

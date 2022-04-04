#include "NativeReanimatedModule.h"
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <react/renderer/uimanager/primitives.h>
#include <functional>
#include <memory>
#include <thread>
#include "EventHandlerRegistry.h"
#include "FeaturesConfig.h"
#include "FrozenObject.h"
#include "JSIStoreValueUser.h"
#include "Mapper.h"
#include "MapperRegistry.h"
#include "MutableValue.h"
#include "ReanimatedHiddenHeaders.h"
#include "RuntimeDecorator.h"
#include "ShareableValue.h"
#include "WorkletEventHandler.h"

using namespace facebook;

namespace reanimated {

void extractMutables(
    jsi::Runtime &rt,
    std::shared_ptr<ShareableValue> sv,
    std::vector<std::shared_ptr<MutableValue>> &res) {
  switch (sv->type) {
    case ValueType::MutableValueType: {
      auto &mutableValue = ValueWrapper::asMutableValue(sv->valueContainer);
      res.push_back(mutableValue);
      break;
    }
    case ValueType::FrozenArrayType:
      for (auto &it : ValueWrapper::asFrozenArray(sv->valueContainer)) {
        extractMutables(rt, it, res);
      }
      break;
    case ValueType::RemoteObjectType:
    case ValueType::FrozenObjectType:
      for (auto &it : ValueWrapper::asFrozenObject(sv->valueContainer)->map) {
        extractMutables(rt, it.second, res);
      }
      break;
    default:
      break;
  }
}

std::vector<std::shared_ptr<MutableValue>> extractMutablesFromArray(
    jsi::Runtime &rt,
    const jsi::Array &array,
    NativeReanimatedModule *module) {
  std::vector<std::shared_ptr<MutableValue>> res;
  for (size_t i = 0, size = array.size(rt); i < size; i++) {
    auto shareable =
        ShareableValue::adapt(rt, array.getValueAtIndex(rt, i), module);
    extractMutables(rt, shareable, res);
  }
  return res;
}

NativeReanimatedModule::NativeReanimatedModule(
    std::shared_ptr<CallInvoker> jsInvoker,
    std::shared_ptr<Scheduler> scheduler,
    std::shared_ptr<jsi::Runtime> rt,
    std::shared_ptr<ErrorHandler> errorHandler,
    std::function<jsi::Value(jsi::Runtime &, const int, const jsi::String &)>
        propObtainer,
    std::shared_ptr<LayoutAnimationsProxy> layoutAnimationsProxy,
    PlatformDepMethodsHolder platformDepMethodsHolder)
    : NativeReanimatedModuleSpec(jsInvoker),
      RuntimeManager(rt, errorHandler, scheduler, RuntimeType::UI),
      mapperRegistry(std::make_shared<MapperRegistry>()),
      eventHandlerRegistry(std::make_shared<EventHandlerRegistry>()),
      requestRender(platformDepMethodsHolder.requestRender),
      propObtainer(propObtainer),
      synchronouslyUpdateUIPropsFunction(
          platformDepMethodsHolder.synchronouslyUpdateUIPropsFunction),
      animatedSensorModule(platformDepMethodsHolder, this),
      configurePropsPlatformFunction(
          platformDepMethodsHolder.configurePropsFunction) {
  auto requestAnimationFrame = [=](FrameCallback callback) {
    frameCallbacks.push_back(callback);
    maybeRequestRender();
  };

  this->layoutAnimationsProxy = layoutAnimationsProxy;

  auto updateProps = [this](
                         jsi::Runtime &rt,
                         const jsi::Value &shadowNodeValue,
                         const jsi::Value &props) {
    this->updateProps(rt, shadowNodeValue, props);
  };

  auto dispatchCommand = [this](
                             jsi::Runtime &rt,
                             const jsi::Value &shadowNodeValue,
                             const jsi::Value &commandNameValue,
                             const jsi::Value &argsValue) {
    this->dispatchCommand(rt, shadowNodeValue, commandNameValue, argsValue);
  };

  auto measure = [this](jsi::Runtime &rt, const jsi::Value &shadowNodeValue) {
    return this->measure(rt, shadowNodeValue);
  };

  RuntimeDecorator::decorateUIRuntime(
      *runtime,
      updateProps,
      dispatchCommand,
      measure,
      requestAnimationFrame,
      platformDepMethodsHolder.getCurrentTime,
      platformDepMethodsHolder.registerSensor,
      platformDepMethodsHolder.unregisterSensor,
      platformDepMethodsHolder.setGestureStateFunction,
      layoutAnimationsProxy);
  onRenderCallback = [this](double timestampMs) {
    this->renderRequested = false;
    this->onRender(timestampMs);
  };
}

void NativeReanimatedModule::installCoreFunctions(
    jsi::Runtime &rt,
    const jsi::Value &valueSetter) {
  this->valueSetter = ShareableValue::adapt(rt, valueSetter, this);
}

jsi::Value NativeReanimatedModule::makeShareable(
    jsi::Runtime &rt,
    const jsi::Value &value) {
  return ShareableValue::adapt(rt, value, this)->getValue(rt);
}

jsi::Value NativeReanimatedModule::makeMutable(
    jsi::Runtime &rt,
    const jsi::Value &value) {
  return ShareableValue::adapt(rt, value, this, ValueType::MutableValueType)
      ->getValue(rt);
}

jsi::Value NativeReanimatedModule::makeRemote(
    jsi::Runtime &rt,
    const jsi::Value &value) {
  return ShareableValue::adapt(rt, value, this, ValueType::RemoteObjectType)
      ->getValue(rt);
}

jsi::Value NativeReanimatedModule::startMapper(
    jsi::Runtime &rt,
    const jsi::Value &worklet,
    const jsi::Value &inputs,
    const jsi::Value &outputs,
    const jsi::Value &updater,
    const jsi::Value &viewDescriptors) {
  static unsigned long MAPPER_ID = 1;

  unsigned long newMapperId = MAPPER_ID++;
  auto mapperShareable = ShareableValue::adapt(rt, worklet, this);
  auto inputMutables =
      extractMutablesFromArray(rt, inputs.asObject(rt).asArray(rt), this);
  auto outputMutables =
      extractMutablesFromArray(rt, outputs.asObject(rt).asArray(rt), this);

  int optimalizationLvl = 0;
  auto optimalization =
      updater.asObject(rt).getProperty(rt, "__optimalization");
  if (optimalization.isNumber()) {
    optimalizationLvl = optimalization.asNumber();
  }
  auto updaterSV = ShareableValue::adapt(rt, updater, this);
  auto viewDescriptorsSV = ShareableValue::adapt(rt, viewDescriptors, this);

  scheduler->scheduleOnUI([=] {
    auto mapperFunction =
        mapperShareable->getValue(*runtime).asObject(*runtime).asFunction(
            *runtime);
    std::shared_ptr<jsi::Function> mapperFunctionPointer =
        std::make_shared<jsi::Function>(std::move(mapperFunction));

    std::shared_ptr<Mapper> mapperPointer = std::make_shared<Mapper>(
        this,
        newMapperId,
        mapperFunctionPointer,
        inputMutables,
        outputMutables);
    if (optimalizationLvl > 0) {
      mapperPointer->enableFastMode(
          optimalizationLvl, updaterSV, viewDescriptorsSV);
    }
    mapperRegistry->startMapper(mapperPointer);
    maybeRequestRender();
  });

  return jsi::Value(static_cast<double>(newMapperId));
}

void NativeReanimatedModule::stopMapper(
    jsi::Runtime &rt,
    const jsi::Value &mapperId) {
  unsigned long id = mapperId.asNumber();
  scheduler->scheduleOnUI([=] {
    mapperRegistry->stopMapper(id);
    maybeRequestRender();
  });
}

jsi::Value NativeReanimatedModule::registerEventHandler(
    jsi::Runtime &rt,
    const jsi::Value &eventHash,
    const jsi::Value &worklet) {
  static unsigned long EVENT_HANDLER_ID = 1;

  unsigned long newRegistrationId = EVENT_HANDLER_ID++;
  auto eventName = eventHash.asString(rt).utf8(rt);
  auto handlerShareable = ShareableValue::adapt(rt, worklet, this);

  scheduler->scheduleOnUI([=] {
    auto handlerFunction =
        handlerShareable->getValue(*runtime).asObject(*runtime).asFunction(
            *runtime);
    auto handler = std::make_shared<WorkletEventHandler>(
        newRegistrationId, eventName, std::move(handlerFunction));
    eventHandlerRegistry->registerEventHandler(handler);
  });

  return jsi::Value(static_cast<double>(newRegistrationId));
}

void NativeReanimatedModule::unregisterEventHandler(
    jsi::Runtime &rt,
    const jsi::Value &registrationId) {
  unsigned long id = registrationId.asNumber();
  scheduler->scheduleOnUI(
      [=] { eventHandlerRegistry->unregisterEventHandler(id); });
}

jsi::Value NativeReanimatedModule::getViewProp(
    jsi::Runtime &rt,
    const jsi::Value &viewTag,
    const jsi::Value &propName,
    const jsi::Value &callback) {
  const int viewTagInt = static_cast<int>(viewTag.asNumber());
  std::string propNameStr = propName.asString(rt).utf8(rt);
  jsi::Function fun = callback.getObject(rt).asFunction(rt);
  std::shared_ptr<jsi::Function> funPtr =
      std::make_shared<jsi::Function>(std::move(fun));

  scheduler->scheduleOnUI([&rt, viewTagInt, funPtr, this, propNameStr]() {
    const jsi::String propNameValue =
        jsi::String::createFromUtf8(rt, propNameStr);
    jsi::Value result = propObtainer(rt, viewTagInt, propNameValue);
    std::string resultStr = result.asString(rt).utf8(rt);

    scheduler->scheduleOnJS([&rt, resultStr, funPtr]() {
      const jsi::String resultValue =
          jsi::String::createFromUtf8(rt, resultStr);
      funPtr->call(rt, resultValue);
    });
  });

  return jsi::Value::undefined();
}

jsi::Value NativeReanimatedModule::enableLayoutAnimations(
    jsi::Runtime &rt,
    const jsi::Value &config) {
  FeaturesConfig::setLayoutAnimationEnabled(config.getBool());
  return jsi::Value::undefined();
}

struct UIManagerBindingPublic {
  void *vtable;
  std::shared_ptr<UIManager> uiManager_;
};

jsi::Value NativeReanimatedModule::initializeForFabric(jsi::Runtime &rt) {
  auto uiManagerBinding = UIManagerBinding::getBinding(rt);
  react_native_assert(
      uiManagerBinding !=
      nullptr); // too early, UIManagerBinding is not registered yet
  auto uiManagerBindingPublic =
      reinterpret_cast<UIManagerBindingPublic *>(&*uiManagerBinding);
  uiManager_ = uiManagerBindingPublic->uiManager_;
  return jsi::Value::undefined();
}

jsi::Value NativeReanimatedModule::configureProps(
    jsi::Runtime &rt,
    const jsi::Value &uiProps,
    const jsi::Value &nativeProps) {
  // Paper
  configurePropsPlatformFunction(rt, uiProps, nativeProps);

  // Fabric
  jsi::Array array = nativeProps.asObject(rt).asArray(rt);
  for (int i = 0; i < array.size(rt); ++i) {
    std::string name = array.getValueAtIndex(rt, i).asString(rt).utf8(rt);
    nativePropNames_.insert(name);
  }

  return jsi::Value::undefined();
}

void NativeReanimatedModule::onEvent(
    std::string eventName,
    jsi::Value &&payload) {
  try {
    eventHandlerRegistry->processEvent(*runtime, eventName, payload);
    mapperRegistry->execute(*runtime);
    if (mapperRegistry->needRunOnRender()) {
      maybeRequestRender();
    }
  } catch (std::exception &e) {
    std::string str = e.what();
    this->errorHandler->setError(str);
    this->errorHandler->raise();
  } catch (...) {
    std::string str = "OnEvent error";
    this->errorHandler->setError(str);
    this->errorHandler->raise();
  }
}

bool NativeReanimatedModule::isAnyHandlerWaitingForEvent(
    std::string eventName) {
  return eventHandlerRegistry->isAnyHandlerWaitingForEvent(eventName);
}

void NativeReanimatedModule::maybeRequestRender() {
  if (!renderRequested) {
    renderRequested = true;
    requestRender(onRenderCallback, *this->runtime);
  }
}

void NativeReanimatedModule::onRender(double timestampMs) {
  try {
    std::vector<FrameCallback> callbacks = frameCallbacks;
    frameCallbacks.clear();
    for (auto &callback : callbacks) {
      callback(timestampMs);
    }
    mapperRegistry->execute(*runtime);

    if (mapperRegistry->needRunOnRender()) {
      maybeRequestRender();
    }
  } catch (std::exception &e) {
    std::string str = e.what();
    this->errorHandler->setError(str);
    this->errorHandler->raise();
  } catch (...) {
    std::string str = "OnRender error";
    this->errorHandler->setError(str);
    this->errorHandler->raise();
  }
}

jsi::Value NativeReanimatedModule::registerSensor(
    jsi::Runtime &rt,
    const jsi::Value &sensorType,
    const jsi::Value &interval,
    const jsi::Value &sensorDataContainer) {
  return animatedSensorModule.registerSensor(
      rt, sensorType, interval, sensorDataContainer);
}

void NativeReanimatedModule::unregisterSensor(
    jsi::Runtime &rt,
    const jsi::Value &sensorId) {
  animatedSensorModule.unregisterSensor(sensorId);
}

// TODO: move to separate file
struct UIManagerPublic {
  void *vtable;
  SharedComponentDescriptorRegistry componentDescriptorRegistry_;
  UIManagerDelegate *delegate_;
  UIManagerAnimationDelegate *animationDelegate_{nullptr};
  RuntimeExecutor const runtimeExecutor_{};
  ShadowTreeRegistry shadowTreeRegistry_{};
  BackgroundExecutor const backgroundExecutor_{};
  ContextContainer::Shared contextContainer_;
};

void NativeReanimatedModule::updateProps(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeValue,
    const jsi::Value &props) {
  ShadowNode::Shared shadowNode = shadowNodeFromValue(rt, shadowNodeValue);

  // TODO: move to separate method
  bool uiPropsOnly = [&]() {
    const jsi::Array propNames = props.asObject(rt).getPropertyNames(rt);
    for (size_t i = 0; i < propNames.size(rt); ++i) {
      const std::string propName =
          propNames.getValueAtIndex(rt, i).asString(rt).utf8(rt);
      bool isNativeProp =
          nativePropNames_.find(propName) != nativePropNames_.end();
      // TODO: std::unordered_set<std::string>::contains?
      if (isNativeProp) {
        return false;
      }
    }
    return true;
  }();

  if (uiPropsOnly) {
    Tag tag = shadowNode->getTag();
    synchronouslyUpdateUIPropsFunction(rt, tag, props);
  } else {
    // TODO: use uiManager_->getNewestCloneOfShadowNode?
    auto rawProps = std::make_unique<RawProps>(rt, props);
    operationsInBatch_.emplace_back(shadowNode, std::move(rawProps));
  }
}

void NativeReanimatedModule::performOperations() {
  if (operationsInBatch_.empty()) {
    return;
  }

  auto copiedOperationsQueue = std::move(operationsInBatch_);
  operationsInBatch_ =
      std::vector<std::pair<ShadowNode::Shared, std::unique_ptr<RawProps>>>();

  // TODO: move shadowTreeRegistry and contextContainer to
  // NativeReanimatedModule fields
  std::shared_ptr<UIManager> uiManager = uiManager_;
  auto uiManagerPublic = reinterpret_cast<UIManagerPublic *>(&*uiManager);
  ShadowTreeRegistry *shadowTreeRegistry =
      &uiManagerPublic->shadowTreeRegistry_;
  std::shared_ptr<const ContextContainer> contextContainer =
      uiManagerPublic->contextContainer_;
  SurfaceId surfaceId = 1; // TODO: handle surface id
  PropsParserContext propsParserContext{surfaceId, *contextContainer};

  shadowTreeRegistry->visit(surfaceId, [&](ShadowTree const &shadowTree) {
    ShadowTreeCommitTransaction transaction =
        [&](RootShadowNode const &oldRootShadowNode) {
          ShadowNode::Unshared newRoot =
              oldRootShadowNode.ShadowNode::clone(ShadowNodeFragment{});

          for (const auto &pair : copiedOperationsQueue) {
            const ShadowNodeFamily &family = pair.first->getFamily();
            react_native_assert(family.getSurfaceId() == 1);

            std::function<ShadowNode::Unshared(ShadowNode const &oldShadowNode)>
                callback = [&](ShadowNode const &oldShadowNode) {
                  Props::Shared newProps =
                      oldShadowNode.getComponentDescriptor().cloneProps(
                          propsParserContext,
                          oldShadowNode.getProps(),
                          *pair.second);

                  ShadowNodeFragment fragment{/* .props = */ newProps};
                  auto clone = oldShadowNode.clone(fragment);
                  setNewestCloneOfShadowNodeFromReanimated(clone);
                  // TODO: what if transaction fails?
                  return clone;
                };

            newRoot = newRoot->cloneTree(family, callback);
            if (!newRoot) { // cloneTree returned ShadowNode::Unshared{nullptr}
              break; // cancel transaction by returning null RootShadowNode
            }
          }

          return std::static_pointer_cast<RootShadowNode>(newRoot);
        };
    ShadowTree::CommitOptions commitOptions{};
    shadowTree.commit(transaction, commitOptions);
  });
}

void NativeReanimatedModule::dispatchCommand(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeValue,
    const jsi::Value &commandNameValue,
    const jsi::Value &argsValue) {
  ShadowNode::Shared shadowNode = shadowNodeFromValue(rt, shadowNodeValue);
  std::string commandName = stringFromValue(rt, commandNameValue);
  folly::dynamic args = commandArgsFromValue(rt, argsValue);

  // UIManager::dispatchCommand is a private method, we cannot call it directly
  std::shared_ptr<UIManager> uiManager = uiManager_;
  auto uiManagerPublic = reinterpret_cast<UIManagerPublic *>(&*uiManager);
  UIManagerDelegate *delegate = uiManagerPublic->delegate_;
  if (!delegate) {
    return;
  }

  delegate->uiManagerDidDispatchCommand(shadowNode, commandName, args);
}

static inline LayoutMetrics getRelativeLayoutMetrics(
    std::shared_ptr<UIManager> uiManager,
    ShadowNode const &shadowNode,
    ShadowNode const *ancestorShadowNode,
    LayoutableShadowNode::LayoutInspectingPolicy policy) {
  // based on implementation from UIManager.cpp

  auto uiManagerPublic = reinterpret_cast<UIManagerPublic *>(&*uiManager);
  ShadowTreeRegistry *shadowTreeRegistry =
      &uiManagerPublic->shadowTreeRegistry_;

  // We might store here an owning pointer to `ancestorShadowNode` to ensure
  // that the node is not deallocated during method execution lifetime.
  auto owningAncestorShadowNode = ShadowNode::Shared{};

  if (!ancestorShadowNode) {
    shadowTreeRegistry->visit(
        shadowNode.getSurfaceId(), [&](ShadowTree const &shadowTree) {
          owningAncestorShadowNode =
              shadowTree.getCurrentRevision().rootShadowNode;
          ancestorShadowNode = owningAncestorShadowNode.get();
        });
  } else {
    // It is possible for JavaScript (or other callers) to have a reference
    // to a previous version of ShadowNodes, but we enforce that
    // metrics are only calculated on most recently committed versions.
    owningAncestorShadowNode =
        uiManager->getNewestCloneOfShadowNode(*ancestorShadowNode);
    ancestorShadowNode = owningAncestorShadowNode.get();
  }

  auto layoutableAncestorShadowNode =
      traitCast<LayoutableShadowNode const *>(ancestorShadowNode);

  if (!layoutableAncestorShadowNode) {
    return EmptyLayoutMetrics;
  }

  return LayoutableShadowNode::computeRelativeLayoutMetrics(
      shadowNode.getFamily(), *layoutableAncestorShadowNode, policy);
}

jsi::Value NativeReanimatedModule::measure(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeValue) {
  // based on implementation from UIManagerBinding.cpp

  auto shadowNode = shadowNodeFromValue(rt, shadowNodeValue);
  // TODO: use uiManager_->getRelativeLayoutMetrics once it's public
  // auto layoutMetrics = uiManager_->getRelativeLayoutMetrics(
  //     *shadowNode, nullptr, {/* .includeTransform = */ true});
  auto layoutMetrics = getRelativeLayoutMetrics(
      uiManager_, *shadowNode, nullptr, {/* .includeTransform = */ true});

  if (layoutMetrics == EmptyLayoutMetrics) {
    return jsi::Value::undefined();
  }
  auto newestCloneOfShadowNode =
      uiManager_->getNewestCloneOfShadowNode(*shadowNode);

  auto layoutableShadowNode =
      traitCast<LayoutableShadowNode const *>(newestCloneOfShadowNode.get());
  facebook::react::Point originRelativeToParent = layoutableShadowNode
      ? layoutableShadowNode->getLayoutMetrics().frame.origin
      : facebook::react::Point();

  auto frame = layoutMetrics.frame;

  jsi::Object result(rt);
  result.setProperty(
      rt, "x", jsi::Value(static_cast<double>(originRelativeToParent.x)));
  result.setProperty(
      rt, "y", jsi::Value(static_cast<double>(originRelativeToParent.y)));
  result.setProperty(
      rt, "width", jsi::Value(static_cast<double>(frame.size.width)));
  result.setProperty(
      rt, "height", jsi::Value(static_cast<double>(frame.size.height)));
  result.setProperty(
      rt, "pageX", jsi::Value(static_cast<double>(frame.origin.x)));
  result.setProperty(
      rt, "pageY", jsi::Value(static_cast<double>(frame.origin.y)));
  return result;
}

} // namespace reanimated

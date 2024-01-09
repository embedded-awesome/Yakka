#pragma once

namespace tf {

// ----------------------------------------------------------------------------

class TopologyBase {

};

// class: Topology
class Topology {

  friend class Executor;
  friend class Runtime;
  friend class Node;

  template <typename T>
  friend class Future;
  
  constexpr static int CLEAN = 0;
  constexpr static int CANCELLED = 1;
  constexpr static int EXCEPTION = 2;

  public:

    template <typename P, typename C>
    Topology(Taskflow&, P&&, C&&);

  private:

    Taskflow& _taskflow;

    std::promise<void> _promise;

    SmallVector<Node*> _sources;

    std::function<bool()> _pred;
    std::function<void()> _call;

    std::atomic<size_t> _join_counter {0};
    std::atomic<int> _state {CLEAN};

    std::exception_ptr _exception {nullptr};

    void _carry_out_promise();
};

// Constructor
template <typename P, typename C>
Topology::Topology(Taskflow& tf, P&& p, C&& c):
  _taskflow(tf),
  _pred {std::forward<P>(p)},
  _call {std::forward<C>(c)} {
}

// Procedure
inline void Topology::_carry_out_promise() {
  _exception ? _promise.set_exception(_exception) : _promise.set_value();
}

}  // end of namespace tf. ----------------------------------------------------

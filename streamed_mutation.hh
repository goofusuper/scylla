/*
 * Copyright (C) 2016 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "mutation_partition.hh"
#include "utils/optimized_optional.hh"

#include <experimental/optional>

namespace stdx = std::experimental;

// mutation_fragments are the objects that streamed_mutation are going to
// stream. They can represent:
//  - a static row
//  - a clustering row
//  - start of range tombstone
//  - end of range rombstone
//
// There exists an ordering (implemented in position_in_partition class) between
// mutation_fragment objects. It reflects the order in which content of
// partition appears in the sstables.

class position_in_partition;

class clustering_row {
    clustering_key_prefix _ck;
    tombstone _t;
    row_marker _marker;
    row _cells;
public:
    explicit clustering_row(clustering_key_prefix ck) : _ck(std::move(ck)) { }
    clustering_row(clustering_key_prefix ck, tombstone t, row_marker marker, row cells)
        : _ck(std::move(ck)), _t(t), _marker(std::move(marker)), _cells(std::move(cells)) { }
    clustering_row(const rows_entry& re)
        : _ck(re.key()), _t(re.row().deleted_at()), _marker(re.row().marker()), _cells(re.row().cells()) { }
    clustering_row(rows_entry&& re)
        : _ck(std::move(re.key())), _t(re.row().deleted_at()), _marker(re.row().marker()), _cells(std::move(re.row().cells())) { }

    clustering_key_prefix& key() { return _ck; }
    const clustering_key_prefix& key() const { return _ck; }

    tombstone tomb() const { return _t; }
    void remove_tombstone() { _t = tombstone(); }

    const row_marker& marker() const { return _marker; }
    row_marker& marker() { return _marker; }

    const row& cells() const { return _cells; }
    row& cells() { return _cells; }

    bool empty() const {
        return !_t && _marker.is_missing() && _cells.empty();
    }

    void apply(const schema& s, clustering_row&& cr) {
        _t.apply(cr._t);
        _marker.apply(std::move(cr._marker));
        _cells.apply(s, column_kind::regular_column, std::move(cr._cells));
    }
    void set_cell(const column_definition& def, atomic_cell_or_collection&& value) {
        _cells.apply(def, std::move(value));
    }
    void apply(row_marker rm) { _marker.apply(std::move(rm)); }
    void apply(tombstone t) { _t.apply(t); }

    void apply(const schema& s, const rows_entry& r) {
        _t.apply(r.row().deleted_at());
        _marker.apply(r.row().marker());
        _cells.apply(s, column_kind::regular_column, r.row().cells());
    }

    position_in_partition position() const;
};

class static_row {
    row _cells;
public:
    static_row() = default;
    explicit static_row(const row& r) : _cells(r) { }
    explicit static_row(row&& r) : _cells(std::move(r)) { }

    row& cells() { return _cells; }

    bool empty() const {
        return _cells.empty();
    }

    void apply(const schema& s, const row& r) {
        _cells.apply(s, column_kind::static_column, r);
    }
    void apply(const schema& s, static_row&& sr) {
        _cells.apply(s, column_kind::static_column, std::move(sr._cells));
    }
    void set_cell(const column_definition& def, atomic_cell_or_collection&& value) {
        _cells.apply(def, std::move(value));
    }

    position_in_partition position() const;
};

class range_tombstone_begin {
    clustering_key_prefix _ck;
    tombstone _t;
    bound_kind _kind;
public:
    range_tombstone_begin(clustering_key_prefix ck, bound_kind kind, tombstone t)
        : _ck(std::move(ck)), _t(t), _kind(kind) { }

    clustering_key_prefix& key() { return _ck; }
    const clustering_key_prefix& key() const { return _ck; }
    bound_kind kind() const { return _kind; }

    bound_view bound() const { return bound_view(_ck, _kind); }

    tombstone tomb() const { return _t; }
    void apply(range_tombstone_begin&& rtb) {
        _t.apply(rtb._t);
    }

    position_in_partition position() const;
};

class range_tombstone_end {
    clustering_key_prefix _ck;
    bound_kind _kind;
public:
    range_tombstone_end(clustering_key_prefix ck, bound_kind kind)
        : _ck(std::move(ck)), _kind(kind) { }

    clustering_key_prefix& key() { return _ck; }
    const clustering_key_prefix& key() const { return _ck; }
    bound_kind kind() const { return _kind; }

    bound_view bound() const { return bound_view(_ck, _kind); }

    position_in_partition position() const;
};

class mutation_fragment {
public:
    enum class kind {
        static_row,
        clustering_row,
        range_tombstone_begin,
        range_tombstone_end,
    };
private:
    union data {
        data() { }
        ~data() { }

        static_row _static_row;
        clustering_row _clustering_row;
        range_tombstone_begin _range_tombstone_begin;
        range_tombstone_end _range_tombstone_end;
    };
private:
    kind _kind;
    std::unique_ptr<data> _data;

    mutation_fragment() = default;
    explicit operator bool() const noexcept { return bool(_data); }
    void destroy_data() noexcept;
    friend class optimized_optional<mutation_fragment>;

    int row_type_weight() const { return !is_static_row(); }
    int bound_kind_weight() const;
    friend class position_in_partition;
public:
    mutation_fragment(static_row&& r);
    mutation_fragment(clustering_row&& r);
    mutation_fragment(range_tombstone_begin&& r);
    mutation_fragment(range_tombstone_end&& r);

    mutation_fragment(const mutation_fragment&) = delete;
    mutation_fragment(mutation_fragment&& other) = default;
    mutation_fragment& operator=(const mutation_fragment&) = delete;
    mutation_fragment& operator=(mutation_fragment&& other) noexcept {
        if (this != &other) {
            this->~mutation_fragment();
            new (this) mutation_fragment(std::move(other));
        }
        return *this;
    }
    ~mutation_fragment() {
        if (_data) {
            destroy_data();
        }
    }

    position_in_partition position() const;

    bool has_key() const { return !is_static_row(); }
    // Requirements: has_key() == true
    const clustering_key_prefix& key() const;

    kind mutation_fragment_kind() const { return _kind; }

    bool is_static_row() const { return _kind == kind::static_row; }
    bool is_clustering_row() const { return _kind == kind::clustering_row; }
    bool is_range_tombstone_begin() const { return _kind == kind::range_tombstone_begin; }
    bool is_range_tombstone_end() const { return _kind == kind::range_tombstone_end; }

    static_row& as_static_row() { return _data->_static_row; }
    clustering_row& as_clustering_row() { return _data->_clustering_row; }
    range_tombstone_begin& as_range_tombstone_begin() { return _data->_range_tombstone_begin; }
    range_tombstone_end& as_range_tombstone_end() { return _data->_range_tombstone_end; }

    const static_row& as_static_row() const { return _data->_static_row; }
    const clustering_row& as_clustering_row() const { return _data->_clustering_row; }
    const range_tombstone_begin& as_range_tombstone_begin() const { return _data->_range_tombstone_begin; }
    const range_tombstone_end& as_range_tombstone_end() const { return _data->_range_tombstone_end; }

    // Requirements: mutation_fragment_kind() == mf.mutation_fragment_kind()
    void apply(const schema& s, mutation_fragment&& mf);

    /*
    template<typename T, typename ReturnType>
    concept bool MutationFragmentConsumer() {
        return requires(T t, static_row sr, clustering_row cr, range_tombstone_begin rtb, range_tombstone_end rte) {
            { t.consume(std::move(sr)) } -> ReturnType;
            { t.consume(std::move(cr)) } -> ReturnType;
            { t.consume(std::move(rtb)) } -> ReturnType;
            { t.consume(std::move(rte)) } -> ReturnType;
        };
    }
    */
    template<typename Consumer>
    auto consume(Consumer& consumer) && {
        switch (_kind) {
        case kind::static_row:
            return consumer.consume(std::move(_data->_static_row));
        case kind::clustering_row:
            return consumer.consume(std::move(_data->_clustering_row));
        case kind::range_tombstone_begin:
            return consumer.consume(std::move(_data->_range_tombstone_begin));
        case kind::range_tombstone_end:
            return consumer.consume(std::move(_data->_range_tombstone_end));
        }
        abort();
    }
};

class position_in_partition {
    int _bound_weight = 0;
    stdx::optional<clustering_key_prefix> _ck;
private:
    // 0 for static row, 1 for clustering row and range tombstones begin/end
    int row_type_weight() const { return !!_ck; }
    int bound_kind_weight() const { return _bound_weight; }
public:
    struct static_row_tag_t { };
    struct clustering_row_tag_t { };
    struct range_tombstone_begin_tag_t { };
    struct range_tombstone_end_tag_t { };

    explicit position_in_partition(static_row_tag_t) { }
    position_in_partition(clustering_row_tag_t, clustering_key_prefix ck)
        : _ck(std::move(ck)) { }
    position_in_partition(range_tombstone_begin_tag_t, bound_view bv)
        : _bound_weight(weight(bv.kind)), _ck(bv.prefix) { }
    position_in_partition(range_tombstone_end_tag_t, bound_view bv)
        : _bound_weight(weight(bv.kind)), _ck(bv.prefix) { }

    clustering_key_prefix& key() {
        return *_ck;
    }
    const clustering_key_prefix& key() const {
        return *_ck;
    }

    class less_compare {
        bound_view::compare _cmp;
    private:
        template<typename T, typename U>
        bool compare(const T& a, const U& b) const {
            auto a_rt_weight = a.row_type_weight();
            auto b_rt_weight = b.row_type_weight();
            if (!a_rt_weight || !b_rt_weight) {
                return a_rt_weight < b_rt_weight;
            }
            return _cmp(a.key(), a.bound_kind_weight(), b.key(), b.bound_kind_weight());
        }
    public:
        less_compare(const schema& s) : _cmp(s) { }
        bool operator()(const position_in_partition& a, const position_in_partition& b) const {
            return compare(a, b);
        }
        bool operator()(const mutation_fragment& a, const mutation_fragment& b) const {
            return compare(a, b);
        }
        bool operator()(const position_in_partition& a, const mutation_fragment& b) const {
            return compare(a, b);
        }
        bool operator()(const mutation_fragment& a, const position_in_partition& b) const {
            return compare(a, b);
        }
        bool operator()(const position_in_partition& a, const rows_entry& b) const {
            return !a.row_type_weight() || _cmp(a.key(), a.bound_kind_weight(), b.key(), 0);
        }
        bool operator()(const rows_entry& a, const position_in_partition& b) const {
            return b.row_type_weight() && _cmp(a.key(), 0, b.key(), b.bound_kind_weight());
        }
        bool operator()(const rows_entry& a, const rows_entry& b) const {
            return _cmp(a.key(), 0, b.key(), 0);
        }
        bool operator()(const range_tombstone_end& a, const mutation_fragment& b) const {
            return b.row_type_weight() && _cmp(a.key(), weight(a.kind()), b.key(), b.bound_kind_weight());
        }
        bool operator()(const range_tombstone& a, const mutation_fragment& b) const {
            return b.row_type_weight() && _cmp(a.start, weight(a.start_kind), b.key(), b.bound_kind_weight());
        }
        bool operator()(const bound_view& a, const rows_entry& b) const {
            return _cmp(a.prefix, weight(a.kind), b.key(), 0);
        }
        bool operator()(const bound_view& a, const mutation_fragment& b) const {
            return b.row_type_weight() && _cmp(a.prefix, weight(a.kind), b.key(), b.bound_kind_weight());
        }
    };
    class equal_compare {
        clustering_key_prefix::equality _equal;
        template<typename T, typename U>
        bool compare(const T& a, const U& b) const {
            return a.row_type_weight() == b.row_type_weight()
                   && (!a.row_type_weight() || (_equal(a.key(), b.key())
                        && a.bound_kind_weight() == b.bound_kind_weight()));
        }
    public:
        equal_compare(const schema& s) : _equal(s) { }
        bool operator()(const position_in_partition& a, const position_in_partition& b) const {
            return compare(a, b);
        }
        bool operator()(const mutation_fragment& a, const mutation_fragment& b) const {
            return compare(a, b);
        }
        bool operator()(const position_in_partition& a, const mutation_fragment& b) const {
            return compare(a, b);
        }
        bool operator()(const mutation_fragment& a, const position_in_partition& b) const {
            return compare(a, b);
        }
        bool operator()(const rows_entry& a, const clustering_row& b) const {
            return _equal(a.key(), b.key());
        }
        bool operator()(const clustering_row& a, const clustering_row& b) const {
            return _equal(a.key(), b.key());
        }
        bool operator()(const clustering_row& a, const rows_entry& b) const {
            return _equal(a.key(), b.key());
        }
    };
};

inline position_in_partition static_row::position() const
{
    return position_in_partition(position_in_partition::static_row_tag_t());
}

inline position_in_partition clustering_row::position() const
{
    return position_in_partition(position_in_partition::clustering_row_tag_t(), _ck);
}

inline position_in_partition range_tombstone_begin::position() const
{
    return position_in_partition(position_in_partition::range_tombstone_begin_tag_t(), bound());
}

inline position_in_partition range_tombstone_end::position() const
{
    return position_in_partition(position_in_partition::range_tombstone_end_tag_t(), bound());
}

template<>
struct move_constructor_disengages<mutation_fragment> {
    enum { value = true };
};
using mutation_fragment_opt = optimized_optional<mutation_fragment>;

// streamed_mutation represents a mutation in a form of a stream of
// mutation_fragments. streamed_mutation emits mutation fragments in the order
// they should appear in the sstables, i.e. static row is always the first one,
// then clustering rows and range tombstones are emitted according to the
// lexicographical ordering of their clustering keys and bounds of the range
// tombstones.
//
// Range tombstones are disjoint, i.e. after emitting
// range_tombstone_begin it is guaranteed that there is going to be a single
// range_tombstone_end before another range_tombstone_begin is emitted.
//
// The ordering of mutation_fragments also guarantees that by the time the
// consumer sees a clustering row it has already received all relevant tombstones.
//
// Partition key and partition tombstone are not streamed and is part of the
// streamed_mutation itself.
class streamed_mutation {
public:
    // streamed_mutation uses batching. The mutation implementations are
    // supposed to fill a buffer with mutation fragments until is_buffer_full()
    // or end of stream is encountered.
    class impl {
    protected:
        // FIXME: use size in bytes of the mutation_fragments
        static constexpr size_t buffer_size = 16;

        schema_ptr _schema;
        dht::decorated_key _key;
        tombstone _partition_tombstone;

        bool _end_of_stream = false;
        circular_buffer<mutation_fragment> _buffer;

        friend class streamed_mutation;
    protected:
        template<typename... Args>
        void push_mutation_fragment(Args&&... args) {
            _buffer.emplace_back(std::forward<Args>(args)...);
        }
    public:
        explicit impl(schema_ptr s, dht::decorated_key dk, tombstone pt)
            : _schema(std::move(s)), _key(std::move(dk)), _partition_tombstone(pt)
        {
            _buffer.reserve(buffer_size);
        }

        virtual ~impl() { }
        virtual future<> fill_buffer() = 0;

        bool is_end_of_stream() const { return _end_of_stream; }
        bool is_buffer_empty() const { return _buffer.empty(); }
        bool is_buffer_full() const { return _buffer.size() >= buffer_size; }

        mutation_fragment pop_mutation_fragment() {
            auto mf = std::move(_buffer.front());
            _buffer.pop_front();
            return mf;
        }

        future<mutation_fragment_opt> operator()() {
            if (is_buffer_empty()) {
                if (is_end_of_stream()) {
                    return make_ready_future<mutation_fragment_opt>();
                }
                return fill_buffer().then([this] { return operator()(); });
            }
            return make_ready_future<mutation_fragment_opt>(pop_mutation_fragment());
        }
    };
private:
    std::unique_ptr<impl> _impl;

    streamed_mutation() = default;
    explicit operator bool() const { return bool(_impl); }
    friend class optimized_optional<streamed_mutation>;
public:
    explicit streamed_mutation(std::unique_ptr<impl> i)
        : _impl(std::move(i)) { }

    const partition_key& key() const { return _impl->_key.key(); }
    const dht::decorated_key decorated_key() const { return _impl->_key; }

    schema_ptr schema() const { return _impl->_schema; }

    tombstone partition_tombstone() const { return _impl->_partition_tombstone; }

    bool is_end_of_stream() const { return _impl->is_end_of_stream(); }
    bool is_buffer_empty() const { return _impl->is_buffer_empty(); }
    bool is_buffer_full() const { return _impl->is_buffer_full(); }

    mutation_fragment pop_mutation_fragment() { return _impl->pop_mutation_fragment(); }

    future<> fill_buffer() { return _impl->fill_buffer(); }

    future<mutation_fragment_opt> operator()() {
        return _impl->operator()();
    }
};

std::ostream& operator<<(std::ostream& os, const streamed_mutation& sm);

template<typename Impl, typename... Args>
streamed_mutation make_streamed_mutation(Args&&... args) {
    return streamed_mutation(std::make_unique<Impl>(std::forward<Args>(args)...));
}

template<>
struct move_constructor_disengages<streamed_mutation> {
    enum { value = true };
};
using streamed_mutation_opt = optimized_optional<streamed_mutation>;

/*
template<typename T>
concept bool StreamedMutationConsumer() {
    return MutationFragmentConsumer<T, stop_iteration>
        && requires(T t, tombstone tomb)
    {
        { t.consume(tomb) } -> stop_iteration;
        t.consume_end_of_stream();
    };
}
*/
template<typename Consumer>
auto consume(streamed_mutation& m, Consumer consumer) {
    return do_with(std::move(consumer), [&m] (Consumer& c) {
        if (c.consume(m.partition_tombstone()) == stop_iteration::yes) {
            return make_ready_future().then([&] { return c.consume_end_of_stream(); });
        }
        return repeat([&m, &c] {
            if (m.is_buffer_empty()) {
                if (m.is_end_of_stream()) {
                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                }
                return m.fill_buffer().then([] { return stop_iteration::no; });
            }
            return make_ready_future<stop_iteration>(m.pop_mutation_fragment().consume(c));
        }).then([&c] {
            return c.consume_end_of_stream();
        });
    });
}

class mutation;

streamed_mutation streamed_mutation_from_mutation(mutation);

//Requires all streamed_mutations to have the same schema.
streamed_mutation merge_mutations(std::vector<streamed_mutation>);
streamed_mutation reverse_streamed_mutation(streamed_mutation);

// range_tombstone_stream is a helper object that simplifies producing a stream
// of range tombstones and merging it with a stream of clustering rows.
// Tombstones are added using apply() and retrieved using get_next().
//
// get_next(const rows_entry&) and get_next(const mutation_fragment&) allow
// merging the stream of tombstones with a stream of clustering rows. If these
// overloads return disengaged optional it means that there is no tombstone
// in the stream that should be emitted before the object given as an argument.
// (And, consequently, if the optional is engaged that tombstone should be
// emitted first). After calling any of these overloads with a mutation_fragment
// which is at some position in partition P no range tombstone can be added to
// the stream which start bound is before that position.
//
// get_next() overload which doesn't take any arguments is used to return the
// remaining tombstones. After it was called no new tombstones can be added
// to the stream.
class range_tombstone_stream {
    const schema& _schema;
    position_in_partition::less_compare _cmp;
    range_tombstone_list _list;
    bool _inside_range_tombstone = false;
private:
    mutation_fragment_opt get_next_start();
    mutation_fragment_opt get_next_end();
public:
    range_tombstone_stream(const schema& s) : _schema(s), _cmp(s), _list(s) { }
    mutation_fragment_opt get_next(const rows_entry&);
    mutation_fragment_opt get_next(const mutation_fragment&);
    mutation_fragment_opt get_next();

    void apply(range_tombstone&& rt) {
        _list.apply(_schema, std::move(rt));
    }
    void apply(const range_tombstone_list& list) {
        _list.apply(_schema, list);
    }
};
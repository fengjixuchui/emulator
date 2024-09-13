#pragma once

#include <span>
#include <vector>
#include <string_view>
#include <stdexcept>
#include <cstring>

namespace utils
{
	class buffer_serializer;
	class buffer_deserializer;

	template <typename T>
	concept Serializable = requires(T a, const T ac, buffer_serializer & serializer, buffer_deserializer & deserializer)
	{
		{ ac.serialize(serializer) } -> std::same_as<void>;
		{ a.deserialize(deserializer) } -> std::same_as<void>;
	};

	/* Use concept instead, to prevent overhead of virtual function calls
	struct serializable
	{
		virtual ~serializable() = default;
		virtual void serialize(buffer_serializer& buffer) const = 0;
		virtual void deserialize(buffer_deserializer& buffer) = 0;
	};
	*/

	namespace detail
	{
		template <typename, typename = void>
		struct has_serialize_function : std::false_type
		{
		};

		template <typename T>
		struct has_serialize_function<T, std::void_t<decltype(serialize(std::declval<buffer_serializer&>(),
		                                                                std::declval<const T&>()))>>
			: std::true_type
		{
		};

		template <typename, typename = void>
		struct has_deserialize_function : std::false_type
		{
		};

		template <typename T>
		struct has_deserialize_function<T, std::void_t<decltype(deserialize(
			                                std::declval<buffer_deserializer&>(), std::declval<T&>()))>>
			: std::true_type
		{
		};
	}

	class buffer_deserializer
	{
	public:
		template <typename T>
		buffer_deserializer(const std::span<T> buffer)
			: buffer_(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size() * sizeof(T))
		{
			static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
		}

		template <typename T>
		buffer_deserializer(const std::vector<T>& buffer)
			: buffer_deserializer(std::span(buffer))
		{
		}

		std::span<const std::byte> read_data(const size_t length)
		{
#ifndef NDEBUG
			const uint64_t real_old_size = this->offset_;
#endif

			if (this->offset_ + length > this->buffer_.size())
			{
				throw std::runtime_error("Out of bounds read from byte buffer");
			}

			const std::span result(this->buffer_.data() + this->offset_, length);
			this->offset_ += length;


#ifndef NDEBUG
			uint64_t old_size{};
			if (this->offset_ + sizeof(old_size) > this->buffer_.size())
			{
				throw std::runtime_error("Out of bounds read from byte buffer");
			}

			memcpy(&old_size, this->buffer_.data() + this->offset_, sizeof(old_size));
			if (old_size != real_old_size)
			{
				throw std::runtime_error("Reading from serialized buffer mismatches written data!");
			}

			this->offset_ += sizeof(old_size);
#endif
		
			return result;
		}

		void read(void* data, const size_t length)
		{
			const auto span = this->read_data(length);
			memcpy(data, span.data(), length);
		}

		template <typename T>
		void read(T& object)
		{
			if constexpr (Serializable<T>)
			{
				object.deserialize(*this);
			}
			else if constexpr (detail::has_deserialize_function<T>::value)
			{
				deserialize(*this, object);
			}
			else if constexpr (std::is_trivially_copyable_v<T>)
			{
				union
				{
					T* type_{};
					void* void_;
				} pointers;

				pointers.type_ = &object;

				this->read(pointers.void_, sizeof(object));
			}
			else
			{
				static_assert(std::false_type::value, "Key must be trivially copyable or implement serializable!");
				std::abort();
			}
		}

		template <typename T>
		T read()
		{
			T object{};
			this->read(object);
			return object;
		}

		template <typename T>
		void read_vector(std::vector<T>& result)
		{
			const auto size = this->read<uint64_t>();
			result.clear();
			result.reserve(size);

			for (uint64_t i = 0; i < size; ++i)
			{
				result.emplace_back(this->read<T>());
			}
		}

		template <typename T>
		std::vector<T> read_vector()
		{
			std::vector<T> result{};
			this->read_vector(result);
			return result;
		}

		template <typename Map>
		void read_map(Map& map)
		{
			using key_type = typename Map::key_type;
			using value_type = typename Map::mapped_type;

			map.clear();

			const auto size = this->read<uint64_t>();

			for (uint64_t i = 0; i < size; ++i)
			{
				auto key = this->read<key_type>();
				auto value = this->read<value_type>();

				map[std::move(key)] = std::move(value);
			}
		}

		template <typename Map>
		Map read_map()
		{
			Map map{};
			this->read_map(map);
			return map;
		}

		template <typename T = char>
		void read_string(std::basic_string<T>& result)
		{
			const auto size = this->read<uint64_t>();

			result.clear();
			result.reserve(size);

			for (uint64_t i = 0; i < size; ++i)
			{
				result.push_back(this->read<T>());
			}
		}

		template <typename T= char>
		std::basic_string<T> read_string()
		{
			std::basic_string<T> result{};
			this->read_string(result);
			return result;
		}

		size_t get_remaining_size() const
		{
			return this->buffer_.size() - offset_;
		}

		std::span<const std::byte> get_remaining_data()
		{
			return this->read_data(this->get_remaining_size());
		}

		size_t get_offset() const
		{
			return this->offset_;
		}

	private:
		size_t offset_{0};
		std::span<const std::byte> buffer_{};
	};

	class buffer_serializer
	{
	public:
		buffer_serializer() = default;

		void write(const void* buffer, const size_t length)
		{
#ifndef NDEBUG
			const uint64_t old_size = this->buffer_.size();
#endif

			const auto* byte_buffer = static_cast<const std::byte*>(buffer);
			this->buffer_.insert(this->buffer_.end(), byte_buffer, byte_buffer + length);

#ifndef NDEBUG
			const auto* security_buffer = reinterpret_cast<const std::byte*>(&old_size);
			this->buffer_.insert(this->buffer_.end(), security_buffer, security_buffer + sizeof(old_size));
#endif
		}

		void write(const buffer_serializer& object)
		{
			const auto& buffer = object.get_buffer();
			this->write(buffer.data(), buffer.size());
		}

		template <typename T>
		void write(const T& object)
		{
			if constexpr (Serializable<T>)
			{
				object.serialize(*this);
			}
			else if constexpr (detail::has_serialize_function<T>::value)
			{
				serialize(*this, object);
			}
			else if constexpr (std::is_trivially_copyable_v<T>)
			{
				union
				{
					const T* type_{};
					const void* void_;
				} pointers;

				pointers.type_ = &object;

				this->write(pointers.void_, sizeof(object));
			}
			else
			{
				static_assert(std::false_type::value, "Key must be trivially copyable or implement serializable!");
				std::abort();
			}
		}

		template <typename T>
		void write_span(const std::span<T> vec)
		{
			this->write(static_cast<uint64_t>(vec.size()));

			for (const auto& v : vec)
			{
				this->write(v);
			}
		}

		template <typename T>
		void write_vector(const std::vector<T> vec)
		{
			this->write_span(std::span(vec));
		}

		template <typename T>
		void write_string(const std::basic_string_view<T> str)
		{
			this->write_span<const T>(str);
		}

		template <typename T>
		void write_string(const std::basic_string<T>& str)
		{
			this->write_string(std::basic_string_view<T>(str));
		}

		template <typename Map>
		void write_map(const Map& map)
		{
			this->write<uint64_t>(map.size());

			for (const auto& entry : map)
			{
				this->write(entry.first);
				this->write(entry.second);
			}
		}

		const std::vector<std::byte>& get_buffer() const
		{
			return this->buffer_;
		}

		std::vector<std::byte> move_buffer()
		{
			return std::move(this->buffer_);
		}

	private:
		std::vector<std::byte> buffer_{};
	};

	template <>
	inline void buffer_deserializer::read<std::string>(std::string& object)
	{
		object = this->read_string<char>();
	}

	template <>
	inline void buffer_deserializer::read<std::wstring>(std::wstring& object)
	{
		object = this->read_string<wchar_t>();
	}

	template <>
	inline void buffer_serializer::write<std::string>(const std::string& object)
	{
		this->write_string(object);
	}

	template <>
	inline void buffer_serializer::write<std::wstring>(const std::wstring& object)
	{
		this->write_string(object);
	}
}
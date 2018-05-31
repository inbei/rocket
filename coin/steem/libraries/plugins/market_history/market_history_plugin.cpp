#include <steem/plugins/market_history/market_history_plugin.hpp>

#include <steem/chain/database.hpp>
#include <steem/chain/index.hpp>
#include <steem/chain/operation_notification.hpp>

#include <fc/io/json.hpp>

namespace steem { namespace plugins { namespace market_history {

namespace detail {

using steem::protocol::fill_order_operation;

class market_history_plugin_impl
{
   public:
      market_history_plugin_impl() :
         _db( appbase::app().get_plugin< steem::plugins::chain::chain_plugin >().db() ) {}
      virtual ~market_history_plugin_impl() {}

      /**
       * This method is called as a callback after a block is applied
       * and will process/index all operations that were applied in the block.
       */
      void update_market_histories( const operation_notification& o );

      chain::database&     _db;
      flat_set<uint32_t>            _tracked_buckets = flat_set<uint32_t>  { 15, 60, 300, 3600, 86400 };
      int32_t                       _maximum_history_per_bucket_size = 1000;
      boost::signals2::connection   post_apply_connection;
};

void market_history_plugin_impl::update_market_histories( const operation_notification& o )
{
   if( o.op.which() == operation::tag< fill_order_operation >::value )
   {
      fill_order_operation op = o.op.get< fill_order_operation >();

      const auto& bucket_idx = _db.get_index< bucket_index >().indices().get< by_bucket >();

      _db.create< order_history_object >( [&]( order_history_object& ho )
      {
         ho.time = _db.head_block_time();
         ho.op = op;
      });

      if( !_maximum_history_per_bucket_size ) return;
      if( !_tracked_buckets.size() ) return;

      for( const auto& bucket : _tracked_buckets )
      {
         auto cutoff = _db.head_block_time() - fc::seconds( bucket * _maximum_history_per_bucket_size );

         auto open = fc::time_point_sec( ( _db.head_block_time().sec_since_epoch() / bucket ) * bucket );
         auto seconds = bucket;

         auto itr = bucket_idx.find( boost::make_tuple( seconds, open ) );
         if( itr == bucket_idx.end() )
         {
            _db.create< bucket_object >( [&]( bucket_object& b )
            {
               b.open = open;
               b.seconds = bucket;

               b.steem.fill( ( op.open_pays.symbol == STEEM_SYMBOL ) ? op.open_pays.amount : op.current_pays.amount );
#ifdef STEEM_ENABLE_SMT
                  b.symbol = ( op.open_pays.symbol == STEEM_SYMBOL ) ? op.current_pays.symbol : op.open_pays.symbol;
#endif
                  b.non_steem.fill( ( op.open_pays.symbol == STEEM_SYMBOL ) ? op.current_pays.amount : op.open_pays.amount );
            });
         }
         else
         {
            _db.modify( *itr, [&]( bucket_object& b )
            {
#ifdef STEEM_ENABLE_SMT
               b.symbol = ( op.open_pays.symbol == STEEM_SYMBOL ) ? op.current_pays.symbol : op.open_pays.symbol;
#endif
               if( op.open_pays.symbol == STEEM_SYMBOL )
               {
                  b.steem.volume += op.open_pays.amount;
                  b.steem.close = op.open_pays.amount;

                  b.non_steem.volume += op.current_pays.amount;
                  b.non_steem.close = op.current_pays.amount;

                  if( b.high() < price( op.current_pays, op.open_pays ) )
                  {
                     b.steem.high = op.open_pays.amount;

                     b.non_steem.high = op.current_pays.amount;
                  }

                  if( b.low() > price( op.current_pays, op.open_pays ) )
                  {
                     b.steem.low = op.open_pays.amount;

                     b.non_steem.low = op.current_pays.amount;
                  }
               }
               else
               {
                  b.steem.volume += op.current_pays.amount;
                  b.steem.close = op.current_pays.amount;

                  b.non_steem.volume += op.open_pays.amount;
                  b.non_steem.close = op.open_pays.amount;

                  if( b.high() < price( op.open_pays, op.current_pays ) )
                  {
                     b.steem.high = op.current_pays.amount;

                     b.non_steem.high = op.open_pays.amount;
                  }

                  if( b.low() > price( op.open_pays, op.current_pays ) )
                  {
                     b.steem.low = op.current_pays.amount;

                     b.non_steem.low = op.open_pays.amount;
                  }
               }
            });

            if( _maximum_history_per_bucket_size > 0 )
            {
               open = fc::time_point_sec();
               itr = bucket_idx.lower_bound( boost::make_tuple( seconds, open ) );

               while( itr->seconds == seconds && itr->open < cutoff )
               {
                  auto old_itr = itr;
                  ++itr;
                  _db.remove( *old_itr );
               }
            }
         }
      }
   }
}

} // detail

market_history_plugin::market_history_plugin() {}
market_history_plugin::~market_history_plugin() {}

void market_history_plugin::set_program_options(
   boost::program_options::options_description& cli,
   boost::program_options::options_description& cfg
)
{
   cfg.add_options()
         ("market-history-bucket-size", boost::program_options::value<string>()->default_value("[15,60,300,3600,86400]"),
           "Track market history by grouping orders into buckets of equal size measured in seconds specified as a JSON array of numbers")
         ("market-history-buckets-per-size", boost::program_options::value<uint32_t>()->default_value(5760),
           "How far back in time to track history for each bucket size, measured in the number of buckets (default: 5760)")
         ;
}

void market_history_plugin::plugin_initialize( const boost::program_options::variables_map& options )
{
   try
   {
      ilog( "market_history: plugin_initialize() begin" );
      my = std::make_unique< detail::market_history_plugin_impl >();

      my->post_apply_connection = my->_db.post_apply_operation.connect( 0, [&]( const operation_notification& o ){ my->update_market_histories( o ); } );
      add_plugin_index< bucket_index        >( my->_db );
      add_plugin_index< order_history_index >( my->_db );

      if( options.count("bucket-size" ) )
      {
         std::string buckets = options["bucket-size"].as< string >();
         my->_tracked_buckets = fc::json::from_string( buckets ).as< flat_set< uint32_t > >();
      }
      if( options.count("history-per-size" ) )
         my->_maximum_history_per_bucket_size = options["history-per-size"].as< uint32_t >();

      wlog( "bucket-size ${b}", ("b", my->_tracked_buckets) );
      wlog( "history-per-size ${h}", ("h", my->_maximum_history_per_bucket_size) );

      ilog( "market_history: plugin_initialize() end" );
   } FC_CAPTURE_AND_RETHROW()
}

void market_history_plugin::plugin_startup() {}

void market_history_plugin::plugin_shutdown()
{
   chain::util::disconnect_signal( my->post_apply_connection );
}

flat_set< uint32_t > market_history_plugin::get_tracked_buckets() const
{
   return my->_tracked_buckets;
}

uint32_t market_history_plugin::get_max_history_per_bucket() const
{
   return my->_maximum_history_per_bucket_size;
}

} } } // steem::plugins::market_history
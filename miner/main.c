#include "bn.h"
#include "keccak256.h"

#include <inttypes.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WORD_BUFFER_BYTES  (2 << 20) // 2 MB
#define WORD_BUFFER_LENGTH (WORD_BUFFER_BYTES / sizeof(struct bn))

#define SAMPLE_INDICES 10

#define THREAD_ITERATIONS 600000

uint32_t coprimes[10];

uint32_t bignum_mod_small( struct bn* b, uint32_t m )
{
   // Compute b % m
   uint64_t tmp = 0;
   size_t i;
   for( int i=BN_ARRAY_SIZE-1; i>=0; i-- )
   {
      tmp = (tmp << 32) | b->array[i];
      tmp %= m;
   }
   return (uint32_t) tmp;
}

void bignum_add_small( struct bn* b, uint32_t n )
{

   uint32_t tmp = b->array[0];
   b->array[0] += n;
   int i = 0;
   while( i < BN_ARRAY_SIZE - 1 && tmp > b->array[i] )
   {
      tmp = b->array[i+1];
      b->array[i+1]++;
      i++;
   }
}

void init_work_constants()
{
   size_t i;

   coprimes[0] = 0x0000fffd;
   coprimes[1] = 0x0000fffb;
   coprimes[2] = 0x0000fff7;
   coprimes[3] = 0x0000fff1;
   coprimes[4] = 0x0000ffef;
   coprimes[5] = 0x0000ffe5;
   coprimes[6] = 0x0000ffdf;
   coprimes[7] = 0x0000ffd9;
   coprimes[8] = 0x0000ffd3;
   coprimes[9] = 0x0000ffd1;
}

struct work_data
{
   uint32_t x[10];
};

void init_work_data( struct work_data* wdata, struct bn* secured_struct_hash )
{
   size_t i;
   struct bn x;
   for( i=0; i<10; i++ )
   {
      wdata->x[i] = bignum_mod_small( secured_struct_hash, coprimes[i] );
   }
}

struct secured_struct
{
   struct bn miner;
   struct bn oo_address;
   struct bn miner_percent;
   struct bn oo_percent;
   struct bn recent_eth_block_number;
   struct bn recent_eth_block_hash;
   struct bn target;
   struct bn pow_height;
};

void hash_secured_struct( struct bn* res, struct secured_struct* ss )
{
   bignum_endian_swap( &ss->target );
   SHA3_CTX c;
   keccak_init( &c );
   keccak_update( &c, (unsigned char*)ss, sizeof(struct secured_struct) );
   keccak_final( &c, (unsigned char*)res );
   bignum_endian_swap( res );
   bignum_endian_swap( &ss->target );
}


void find_and_xor_word( struct bn* result, uint32_t x, uint32_t* coefficients, struct bn* word_buffer )
{
   uint64_t y = coefficients[4];
   y *= x;
   y += coefficients[3];
   y %= WORD_BUFFER_LENGTH - 1;
   y *= x;
   y += coefficients[2];
   y %= WORD_BUFFER_LENGTH - 1;
   y *= x;
   y += coefficients[1];
   y %= WORD_BUFFER_LENGTH - 1;
   y *= x;
   y += coefficients[0];
   y %= WORD_BUFFER_LENGTH - 1;
   bignum_xor( result, word_buffer + y, result );
}


void work( struct bn* result, struct bn* secured_struct_hash, struct bn* nonce, struct bn* word_buffer )
{
   struct work_data wdata;
   init_work_data( &wdata, secured_struct_hash );

   struct bn x;
   struct bn y;
   struct bn tmp;

   bignum_assign( result, secured_struct_hash ); // result = secured_struct_hash;

   uint32_t coefficients[5];

   int i;
   for( i = 0; i < sizeof(coefficients) / sizeof(uint32_t); ++i )
   {
      coefficients[i] = 1 + bignum_mod_small( nonce, coprimes[i] );
   }

   for( i = 0; i < sizeof(coprimes) / sizeof(uint32_t); ++i )
   {
      find_and_xor_word( result, wdata.x[i], coefficients, word_buffer );
   }
}


int main( int argc, char** argv )
{
   struct bn* word_buffer = malloc( WORD_BUFFER_BYTES );
   struct bn seed;

   char bn_str[78];

   SHA3_CTX c;

   keccak_init( &c );
   keccak_update( &c, (unsigned char*)"This is the seed.", 17 );
   keccak_final( &c, (unsigned char*)&seed );

   unsigned char index_padding[24];
   memset( index_padding, 0, sizeof(index_padding) );

   struct bn bn_i;

   // Procedurally generate word buffer w[i] from a seed
   // Each word buffer element is computed by w[i] = H(seed, i)
   for( unsigned long i = 0; i < WORD_BUFFER_LENGTH; i++ )
   {
      keccak_init( &c );
      keccak_update( &c, (unsigned char*)&seed, sizeof(seed) );
      bignum_from_int( &bn_i, i );
      bignum_endian_swap( &bn_i );
      keccak_update( &c, (unsigned char*)&bn_i, sizeof(struct bn) );
      keccak_final( &c, (unsigned char*)(word_buffer + i) );
      bignum_endian_swap( word_buffer + i );
   }

   init_work_constants();

   struct secured_struct ss;

   keccak_init( &c );
   keccak_update( &c, (unsigned char*)"miner", 5 );
   keccak_final( &c, (unsigned char*)&ss.miner );
   bignum_endian_swap( &ss.miner );
   bignum_from_int( &ss.miner_percent, 9500 );
   keccak_init( &c );
   keccak_update( &c, (unsigned char*)"oo_address", 10 );
   keccak_final( &c, (unsigned char*)&ss.oo_address );
   bignum_from_int( &ss.oo_percent, 500 );
   bignum_init( &ss.recent_eth_block_number );
   bignum_init( &ss.recent_eth_block_hash );
   bignum_init( &ss.target );
   bignum_dec( &ss.target );
   bignum_rshift( &ss.target, &ss.target, 32 );
   bignum_init( &ss.pow_height );

   struct bn secured_struct_hash;
   hash_secured_struct( &secured_struct_hash, &ss );

   struct bn nonce;
   bignum_init( &nonce );

   struct bn result, t_nonce, t_result, s_nonce;
   bool stop = false;

   bignum_assign( &s_nonce, &nonce );
   uint32_t hash_report_counter = 0;
   time_t timer;
   struct tm* timeinfo;
   char time_str[20];

   uint64_t thread_iterations = THREAD_ITERATIONS;
   uint64_t hash_limit = ~0;
   uint64_t hashes = 0;

   bignum_init( &result );

   #pragma omp parallel private(t_nonce, t_result)
   {
      while( !stop && hashes <= hash_limit )
      {
         #pragma omp critical
         {
            if( omp_get_thread_num() == 0 )
            {
               if( hash_report_counter >= 10 )
               {
                  time( &timer );
                  timeinfo = localtime( &timer );
                  strftime( time_str, sizeof(time_str), "%FT%T", timeinfo );
                  fprintf( stdout, "H: %s %" PRId64 ";\n", time_str, hashes );
                  hash_report_counter = 0;
               }
               else
               {
                  hash_report_counter++;
               }

            }
            bignum_assign( &t_nonce, &s_nonce );
            bignum_add_small( &s_nonce, thread_iterations );
            hashes += thread_iterations;
         }

         for( uint64_t i = 0; i < thread_iterations; i++ )
         {
            work( &t_result, &secured_struct_hash, &t_nonce, word_buffer );

            if( bignum_cmp( &t_result, &ss.target ) <= 0)
            {
               #pragma omp crticial
               {
                  // Two threads could find a valid proof at the same time (unlikely, but possible).
                  // We want to return the more difficult proof
                  if( !stop )
                  {
                     stop = true;
                     bignum_assign( &result, &t_result );
                     bignum_assign( &nonce, &t_nonce );
                  }
                  else if( bignum_cmp( &t_result, &result ) < 0 )
                  {
                     bignum_assign( &result, &t_result );
                     bignum_assign( &nonce, &t_nonce );
                  }
               }
            }
            else
               bignum_inc( &t_nonce );
         }
      }
   }

   if( bignum_is_zero( &result ) )
   {
      fprintf( stdout, "F:1;\n" );
   }
   else
   {
      bignum_to_string( &nonce, bn_str, sizeof(bn_str), false );
      fprintf( stdout, "N: %s;\n", bn_str );
   }

   bignum_to_string( &nonce, bn_str, sizeof(bn_str), false );
   printf( "Nonce: %s\n", bn_str );

   bignum_to_string( &result, bn_str, sizeof(bn_str), true );
   printf( "Proof: 0x%s\n", bn_str );

   bignum_to_string( &s_nonce, bn_str, sizeof(bn_str), false );
   printf( "Total Hashes: %s\n", bn_str );
}

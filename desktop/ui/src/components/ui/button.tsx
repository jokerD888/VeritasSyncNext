import { Slot } from "@radix-ui/react-slot";
import { cva, type VariantProps } from "class-variance-authority";
import type { ButtonHTMLAttributes } from "react";
import { cn } from "@/lib/utils";

const buttonVariants = cva(
  "inline-flex h-10 items-center justify-center gap-2 rounded-lg px-4 text-sm font-semibold transition focus-visible:outline-none focus-visible:ring-3 focus-visible:ring-blue-200 disabled:pointer-events-none disabled:opacity-50",
  {
    variants: {
      variant: {
        default: "bg-brand text-white shadow-[0_8px_16px_rgba(36,107,254,.22)] hover:bg-[#1558df]",
        secondary: "border border-line bg-white text-ink hover:border-[#abc4fb] hover:bg-[#f8faff]",
        ghost: "text-[#536889] hover:bg-[#edf3ff] hover:text-brand",
        danger: "text-[#c13948] hover:bg-[#fff0f1]"
      },
      size: { default: "h-10 px-4", sm: "h-8 px-3 text-xs", icon: "h-10 w-10 p-0" }
    },
    defaultVariants: { variant: "default", size: "default" }
  }
);

export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement>, VariantProps<typeof buttonVariants> { asChild?: boolean }

export function Button({ className, variant, size, asChild = false, ...props }: ButtonProps) {
  const Component = asChild ? Slot : "button";
  return <Component className={cn(buttonVariants({ variant, size }), className)} {...props} />;
}
